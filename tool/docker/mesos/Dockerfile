#/**
# * Licensed to the Apache Software Foundation (ASF) under one
# * or more contributor license agreements.  See the NOTICE file
# * distributed with this work for additional information
# * regarding copyright ownership.  The ASF licenses this file
# * to you under the Apache License, Version 2.0 (the
# * "License"); you may not use this file except in compliance
# * with the License.  You may obtain a copy of the License at
# *
# *     http://www.apache.org/licenses/LICENSE-2.0
# *
# * Unless required by applicable law or agreed to in writing, software
# * distributed under the License is distributed on an "AS IS" BASIS,
# * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# * See the License for the specific language governing permissions and
# * limitations under the License.
# */

#build Mesos+Hadoop on top of SINGA

FROM singa/gpu
MAINTAINER NVIDIA CORPORATION <digits@nvidia.com>
#this label is need for mesos to distinguish  GPU supported dockers
LABEL com.nvidia.volumes.needed="nvidia_driver"


#COPY incubator-singa/tool/docker/mesos/.bashrc /root/.bashrc
COPY incubator-singa/tool/docker/mesos/install.sh /opt/install.sh
COPY incubator-singa/tool/mesos/* /root/incubator-singa/tool/mesos/

#Install libraries for building mesos
RUN apt-get update && apt-get install -y \
        tar wget git cmake  \
        openjdk-7-jdk \
        build-essential \
        python-dev libcurl4-nss-dev libsasl2-dev \
        libsasl2-modules maven libapr1-dev libsvn-dev \
        libelf-dev \
        autoconf automake libtool


#Installing Mesos
RUN cd /opt && source ./install.sh

#to be removed after SINGA-11 is merged (pulling from Anh's branch instead of pulling from the master)
#RUN source ~/.bashrc && mkdir /root/mesos && cd /root/mesos && git clone https://github.com/ug93tad/incubator-singa && cd incubator-singa && git checkout SINGA-11 && cp -r tool/mesos /root/incubator-singa/tool/ && cd /root/incubator-singa/tool/mesos && make

# update SINGA, but keep the examples directory
#Compile singa scheduler with support of mesos, hdfs
RUN source ~/.bashrc && cd /root/incubator-singa/tool/mesos && make

#copy configuration of hadoop to use them as datanode or namenode
COPY incubator-singa/tool/docker/mesos/*.xml /opt/hadoop-2.6.0/etc/hadoop/

#Copy ssh key so that singa process can ssh into the docker container
RUN ssh-keygen -t dsa -P '' -f ~/.ssh/id_dsa && cat ~/.ssh/id_dsa.pub >> ~/.ssh/authorized_keys && echo 'StrictHostKeyChecking no' >> ~/.ssh/config
