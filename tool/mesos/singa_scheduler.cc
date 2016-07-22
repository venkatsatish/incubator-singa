/************************************************************
*
* Licensed to the Apache Software Foundation (ASF) under one
* or more contributor license agreements.  See the NOTICE file
* distributed with this work for additional information
* regarding copyright ownership.  The ASF licenses this file
* to you under the Apache License, Version 2.0 (the
* "License"); you may not use this file except in compliance
* with the License.  You may obtain a copy of the License at
*
*   http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing,
* software distributed under the License is distributed on an
* "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
* KIND, either express or implied.  See the License for the
* specific language governing permissions and limitations
* under the License.
*
*************************************************************/
#include "singa/proto/job.pb.h"
#include "singa/proto/singa.pb.h"
#include "./scheduler.pb.h"
#include "singa/utils/common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <iostream>
#include <fstream>
#include <hdfs.h>
#include <mesos/scheduler.hpp>
#include <google/protobuf/text_format.h>
#include <glog/logging.h>
/**
 * \file singa_scheduler.cc implements a framework for managing SINGA jobs.
 *
 * The scheduler takes a job configuration file [file] and performs the following:
 *	1. Parse the config file to determine the required resources.
 *	2. [Optional] Copy singa.conf to the HDFS: /singa/singa.conf.
 *			2.1. Raise error if singa.conf is NOT FOUND on HDFS
 *	3. Wait for offers from the Mesos master until enough is acquired.
 *	4. Keep an increasing counter of job ID.
 *	5. Write [file] to HDFS: /singa/job_ID/job.conf
 *	6. Start the task:
 *			+ Set URI in the TaskInfo message to point to the config files on HDFS:
							/singa/singa.conf
							/singa/Job_ID/job.conf
 *			+ Set the executable command: singa-run.sh -conf ./job.conf
 *
 *	We assume that singa-run.sh is include in $PATH variable at all nodes.
 *	(Else, we set the executable to its full path)
 *	./job.conf is relative path pointing to the current sandbox directory created
 *	dynamically by Mesos.
 *
 *
 * Scheduling:
 *	Each SINGA job requires certain resources represented by: (1) number of workers, (2) number of worker groups
 *	and (3) number of worker per process. The resources offered by Mesos contains (1) number of host, (2) number of CPUs
 *	at each host and (3) memory available at each hosts.
 *
 *	Our scheduler performs simply task assignment which guarantees that each process runs an entire work group,
 *	and each takes all the memory offered by the slave. We assume that each slave runs ONE process, that is the following
 *	condition holds:
 *		nCPUs_per_host >= nWorkersPerProcess+nServersPerProcess //if seperate
										 >= max(nWorkerPerProcess, nServersPerProcess) // if attached
 */
using std::string;
using mesos::SchedulerDriver;
using std::vector;
using std::map;

const char usage[] = " singa_scheduler <job_conf> [-scheduler_conf global_config] [-singa_conf singa_config] \n"
                      " job_conf: job configuration file\n"
                      " -scheduler_conf: optional, system-wide configuration file\n"
                      " -singa_conf: optional, singa global configuration file\n";

const char SINGA_CONFIG[] = "../../conf/singa.conf";
const char DEFAULT_SCHEDULER_CONF[] = "scheduler.conf";
class SingaScheduler: public mesos::Scheduler {
 public:
    /**
     * Constructor, used when [sing_conf] is not given. Raise error if /singa/singa.conf is not found
     * on HDFS.
     *
     * @param namenode	address of HDFS namenode
     * @param job_conf_file	job configuration file
     * @param jc		job counter
     */
    SingaScheduler(string namenode, string job_conf_file, int jc):
      job_conf_file_(job_conf_file), nhosts_(0), namenode_(namenode), is_running_(false), job_counter_(jc), task_counter_(0) {

        ReadProtoFromTextFile(job_conf_file_.c_str(), &job_conf_);
    }
    /**
     * Constructor. It overwrites /singa/singa.conf on HDFS (created a new one if necessary).
     * The file contains zookeeper_host and log_dir values
     * It also parses the JobProto from job_config file
     *
     * @param namenode	address of HDFS namenode
     * @param singa_conf	singa global configuration file
     * @param job_conf_file	job configuration file
     */
    SingaScheduler(string namenode, string job_conf_file, string singa_conf, int jc)
      : job_conf_file_(job_conf_file), nhosts_(0), namenode_(namenode), is_running_(false), job_counter_(jc), task_counter_(0) {
	
        ReadProtoFromTextFile(job_conf_file_.c_str(), &job_conf_);
      }
    virtual void registered(SchedulerDriver *driver,
        const mesos::FrameworkID& frameworkId,
        const mesos::MasterInfo& masterInfo) {
    }

    virtual void reregistered(SchedulerDriver *driver,
        const mesos::MasterInfo& masterInfo) {
    }

    virtual void disconnected(SchedulerDriver *driver) {
    }

    /**
     * Handle resource offering from Mesos scheduler. It implements the simple/naive
     * scheduler:
     * + For each offer that contains enough CPUs, adds new tasks to the list
     * + Launch all the tasks when reaching the required number of tasks (nworkers_groups + nserver_groups).
     */
    virtual void resourceOffers(SchedulerDriver* driver, const std::vector<mesos::Offer>& offers) {
      // do nothing if the task is already running
      if (is_running_)
        return;

      for (int i = 0; i < offers.size(); i++) {
        const mesos::Offer offer = offers[i];
        // check for resource and create temporary tasks
        int cpus = 0, mem = 0;
        int nresources = offer.resources().size();

        for (int r = 0; r < nresources; r++) {
          const mesos::Resource& resource = offer.resources(r);
          if (resource.name() == "cpus"
              && resource.type() == mesos::Value::SCALAR)
            cpus = resource.scalar().value();
          else if (resource.name() == "mem"
              && resource.type() == mesos::Value::SCALAR)
            mem = resource.scalar().value();
        }

        if (!check_resources(cpus))
          break;

        vector<mesos::TaskInfo> *new_tasks = new vector<mesos::TaskInfo>();
        mesos::TaskInfo task;
        task.set_name("SINGA");

        char string_id[100];
        snprintf(string_id, 100, "SINGA_%d", nhosts_);
        task.mutable_task_id()->set_value(string_id);
        task.mutable_slave_id()->MergeFrom(offer.slave_id());

        mesos::Resource *resource = task.add_resources();
        resource->set_name("cpus");
        resource->set_type(mesos::Value::SCALAR);
        // take only nworkers_per_group CPUs
        resource->mutable_scalar()->set_value(job_conf_.cluster().nworkers_per_group());

        resource = task.add_resources();
        resource->set_name("mem");
        resource->set_type(mesos::Value::SCALAR);
        // take all the memory
        resource->mutable_scalar()->set_value(mem);

        // store in temporary map
        new_tasks->push_back(task);
        tasks_[offer.id().value()] = new_tasks;
        hostnames_[offer.id().value()] = offer.hostname();
        nhosts_++;
      }

      if (nhosts_>= job_conf_.cluster().nworker_groups()) {
        LOG(INFO) << "Acquired enough resources: "
          << job_conf_.cluster().nworker_groups()*job_conf_.cluster().nworkers_per_group()
          << " CPUs over " << job_conf_.cluster().nworker_groups() << " hosts. Launching tasks ... ";


        // launch tasks
        for (map<string, vector<mesos::TaskInfo>*>::iterator it =
            tasks_.begin(); it != tasks_.end(); ++it) {
          prepare_tasks(it->second, hostnames_[it->first], job_counter_, path);
          mesos::OfferID newId;
          newId.set_value(it->first);
          LOG(INFO) << "Launching task with offer ID = " << newId.value();
          driver->launchTasks(newId, *(it->second));
          task_counter_++;
          if (task_counter_>= job_conf_.cluster().nworker_groups())
            break;
        }

        job_counter_++;
        is_running_ = true;
      }
    }

    virtual void offerRescinded(SchedulerDriver *driver,
        const mesos::OfferID& offerId) {
    }

    virtual void statusUpdate(SchedulerDriver* driver,
        const mesos::TaskStatus& status) {
      if (status.state() == mesos::TASK_FINISHED)
        task_counter_--;

      if (task_counter_ == 0) {
        driver->stop();
      } else if (status.state() == mesos::TASK_FAILED) {
        LOG(ERROR) << "TASK FAILED !!!!";
        driver->abort();
      }
    }

    virtual void frameworkMessage(SchedulerDriver* driver,
        const mesos::ExecutorID& executorId, const mesos::SlaveID& slaveId,
        const string& data) {
    }

    virtual void slaveLost(SchedulerDriver* driver,
        const mesos::SlaveID& slaveId) {
    }

    virtual void executorLost(SchedulerDriver* driver,
        const mesos::ExecutorID& executorId, const mesos::SlaveID& slaveId,
        int status) {
    }

    virtual void error(SchedulerDriver* driver, const string& message) {
      LOG(ERROR) << "ERROR !!! " << message;
    }

 private:
    /**
     * Helper function that initialize TaskInfo with the correct URI and command
     */
    void prepare_tasks(vector<mesos::TaskInfo> *tasks, string hostname, int job_id, string job_conf) {
      char path_sys_config[512], path_job_config[512];
      // path to singa.conf
      snprintf(path_sys_config, 512, "https://s3.amazonaws.com/skatta/DeepLearning/singa.conf");
      snprintf(path_job_config, 512, "https://s3.amazonaws.com/skatta/DeepLearning/job.conf");
      char command[512];
      snprintf(command,512,"/root/incubator-singa/singa -conf ./job.conf -singa_conf ./singa.conf -singa_job %d -host `hostname -I | cut -d ' ' -f1`",job_id)
	// Use Docker to run the task.
      mesos::ContainerInfo containerInfo;
      containerInfo.set_type(mesos::ContainerInfo::DOCKER);

      mesos::ContainerInfo::DockerInfo dockerInfo;
      dockerInfo.set_image("venkatsatishkatta/test");

      containerInfo.mutable_docker()->CopyFrom(dockerInfo);
    //  task.mutable_container()->CopyFrom(containerInfo);

      for (int i=0; i < tasks->size(); i++) {
        mesos::CommandInfo *comm = (tasks->at(i)).mutable_command();
        comm->add_uris()->set_value(path_sys_config);
        comm->add_uris()->set_value(path_job_config);
        comm->set_shell(false);
	comm->set_value("/root/incubator-singa/singa");
	comm->add_arguments("-conf");
	comm->add_arguments("./job.conf");
	comm->add_arguments("-singa_conf");
	comm->add_arguments("./singa.conf");
	comm->add_arguments("-singa_job");
	comm->add_arguments(std::to_string(job_id));
	comm->add_arguments("-host");
	comm->add_arguments(hostname.c_str());
        (tasks->at(i)).mutable_container()->CopyFrom(containerInfo);
	}
     }
    /**
     * Helper function, check if the offered CPUs satisfies the resource requirements
     * @param ncpus:	number of cpus offer at this host
     * @return true		when ncpus >= (nWorkersPerProcess + nServersPerProcess) if workers and servers are separated
     *								or when cpus >= max(nWorkersPerProcess, nServersPerProcess) if they are not.
     */
    bool check_resources(int ncpus) {
      int n1 = job_conf_.cluster().nworkers_per_procs();
      int n2 = job_conf_.cluster().nservers_per_procs();
      LOG(INFO) << "n1 = " << n1 << " n2 = " << n2 << " ncpus = " << ncpus;
      return job_conf_.cluster().server_worker_separate()? ncpus >= (n1+n2) : ncpus >= (n1 > n2 ? n1 : n2);
    }

    int job_counter_, task_counter_;

    // true if the job has been launched
    bool is_running_;
    singa::JobProto job_conf_;
    // total number of hosts required
    int nhosts_;
    // temporary map of tasks: <offerID, TaskInfo>
    map<string, vector<mesos::TaskInfo>*> tasks_;
    // temporary map of offerID to slave IP addresses
    map<string, string> hostnames_;
    // SINGA job config file
    string job_conf_file_;
    // Server address
    string namenode_;
};
int main(int argc, char** argv) {
  FLAGS_logtostderr = 1;
  int status = mesos::DRIVER_RUNNING;
  SingaScheduler *scheduler;
  if (!(argc == 2 || argc == 4 || argc == 6)) {
    std::cout << usage << std::endl;
    return 1;
  }

  int scheduler_conf_idx = 0;
  int singa_conf_idx = 0;
  for (int i=1; i < argc-1; i++) {
    if (strcmp(argv[i], "-scheduler_conf") == 0)
      scheduler_conf_idx = i+1;
    if (strcmp(argv[i], "-singa_conf") == 0)
      singa_conf_idx = i+1;
  }

  SchedulerProto msg;
  if (scheduler_conf_idx)
    singa::ReadProtoFromTextFile((const char*)argv[scheduler_conf_idx], &msg);
  else
    singa::ReadProtoFromTextFile(DEFAULT_SCHEDULER_CONF, &msg);

  if (!singa_conf_idx)
    scheduler = new SingaScheduler(msg.namenode(), string(argv[1]), msg.job_counter());
  else
    scheduler = new SingaScheduler(msg.namenode(), string(argv[1]), string(argv[singa_conf_idx]), msg.job_counter());

  msg.set_job_counter(msg.job_counter()+1);
  if (scheduler_conf_idx)
    singa::WriteProtoToTextFile(msg, (const char*)argv[scheduler_conf_idx]);
  else
    singa::WriteProtoToTextFile(msg, DEFAULT_SCHEDULER_CONF);

  LOG(INFO) << "Scheduler initialized";
  mesos::FrameworkInfo framework;
  framework.set_user("");
  framework.set_name("SINGA");

  SchedulerDriver *driver = new mesos::MesosSchedulerDriver(scheduler, framework, msg.master().c_str());
  LOG(INFO) << "Starting SINGA framework...";
    status = driver->run();
    driver->stop();
  LOG(INFO) << "Stoping SINGA framework...";

  return status == mesos::DRIVER_STOPPED ? 0 : 1;
}
