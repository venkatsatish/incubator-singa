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
/*
 * This file include code from rnnlmlib-0.4 under BSD new license.
 * Copyright (c) 2010-2012 Tomas Mikolov
 * Copyright (c) 2013 Cantab Research Ltd
 * All rights reserved.
 */


//
// This code creates DataShard for RNNLM dataset.
// The RNNLM dataset could be downloaded at
//    http://www.rnnlm.org/
//
// Usage:
//    create_shard.bin -train [train_file] -valid [valid_file]
//                     -test [test_file] -class_size [# of classes]

#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <cmath>
#include <algorithm>
#include <fstream>

#include "singa/io/store.h"
#include "singa/utils/common.h"
#include "singa/proto/common.pb.h"
#include "./rnnlm.pb.h"

#define MAX_STRING 100
#define BUFFER_LEN 32
#define NL_STRING  "</s>"

using std::string;
using std::max;
using std::min;

struct vocab_word {
  int cn;
  char word[MAX_STRING];
  int class_index;
};

struct vocab_word *vocab;
int vocab_max_size;
int vocab_size;
int *vocab_hash;
int vocab_hash_size;
int debug_mode;
int old_classes;
int *class_start;
int *class_end;
int class_size;

char train_file[MAX_STRING];
char valid_file[MAX_STRING];
char test_file[MAX_STRING];

int valid_mode;
int test_mode;

unsigned int getWordHash(char *word) {
  unsigned int hash, a;

  hash = 0;
  for (a = 0; a < strlen(word); a++) hash = hash * 237 + word[a];
  hash = hash % vocab_hash_size;

  return hash;
}

int searchVocab(char *word) {
  int a;
  unsigned int hash;

  hash = getWordHash(word);

  if (vocab_hash[hash] == -1) return -1;
  if (!strcmp(word, vocab[vocab_hash[hash]].word)) return vocab_hash[hash];

  for (a = 0; a < vocab_size; a++) {   // search in vocabulary
    if (!strcmp(word, vocab[a].word)) {
      vocab_hash[hash] = a;
      return a;
    }
  }

  return -1;   // return OOV if not found
}

int addWordToVocab(char *word) {
  unsigned int hash;

  snprintf(vocab[vocab_size].word, strlen(word)+1, "%s", word);
  vocab[vocab_size].cn = 0;
  vocab_size++;

  if (vocab_size + 2 >= vocab_max_size) {   // reallocate memory if needed
    vocab_max_size += 100;
    vocab = (struct vocab_word *) realloc(
        vocab,
        vocab_max_size * sizeof(struct vocab_word));
  }

  hash = getWordHash(word);
  vocab_hash[hash] = vocab_size - 1;

  return vocab_size - 1;
}

void readWord(char *word, FILE *fin) {
  int a = 0, ch;

  while (!feof(fin)) {
    ch = fgetc(fin);

    if (ch == 13) continue;

    if ((ch == ' ') || (ch == '\t') || (ch == '\n')) {
      if (a > 0) {
        if (ch == '\n') ungetc(ch, fin);
        break;
      }

      if (ch == '\n') {
        snprintf(word, strlen(NL_STRING) + 1,
            "%s", const_cast<char *>(NL_STRING));
        return;
      } else {
        continue;
      }
    }

    word[a] = static_cast<char>(ch);
    a++;

    if (a >= MAX_STRING) {
      // printf("Too long word found!\n");   //truncate too long words
      a--;
    }
  }
  word[a] = 0;
}

void sortVocab() {
  int a, b, max;
  vocab_word swap;

  for (a = 1; a < vocab_size; a++) {
    max = a;
    for (b = a + 1; b < vocab_size; b++)
      if (vocab[max].cn < vocab[b].cn) max = b;

    swap = vocab[max];
    vocab[max] = vocab[a];
    vocab[a] = swap;
  }
}

int learnVocabFromTrainFile() {
  char word[MAX_STRING];
  FILE *fin;
  int a, i, train_wcn;

  for (a = 0; a < vocab_hash_size; a++) vocab_hash[a] = -1;

  fin = fopen(train_file, "rb");

  vocab_size = 0;

  addWordToVocab(const_cast<char *>(NL_STRING));

  train_wcn = 0;
  while (1) {
    readWord(word, fin);
    if (feof(fin)) break;

    train_wcn++;

    i = searchVocab(word);
    if (i == -1) {
      a = addWordToVocab(word);
      vocab[a].cn = 1;
    } else {
      vocab[i].cn++;
    }
  }

  sortVocab();

  if (debug_mode > 0) {
    printf("Vocab size: %d\n", vocab_size);
    printf("Words in train file: %d\n", train_wcn);
  }

  fclose(fin);
  return 0;
}

int splitClasses() {
  double df, dd;
  int i, a, b;

  df = 0;
  dd = 0;
  a = 0;
  b = 0;

  class_start = reinterpret_cast<int *>(calloc(class_size, sizeof(int)));
  memset(class_start, 0x7f, sizeof(int) * class_size);
  class_end = reinterpret_cast<int *>(calloc(class_size, sizeof(int)));
  memset(class_end, 0, sizeof(int) * class_size);

  if (old_classes) {    // old classes
    for (i = 0; i < vocab_size; i++)
      b += vocab[i].cn;
    for (i = 0; i < vocab_size; i++) {
      df += vocab[i].cn / static_cast<double>(b);
      if (df > 1) df = 1;
      if (df > (a + 1) / static_cast<double>(class_size)) {
        vocab[i].class_index = a;
        if (a < class_size - 1) a++;
      } else {
        vocab[i].class_index = a;
      }
    }
  } else {            // new classes
    for (i = 0; i < vocab_size; i++)
      b += vocab[i].cn;
    for (i = 0; i < vocab_size; i++)
      dd += sqrt(vocab[i].cn / static_cast<double>(b));
    for (i = 0; i < vocab_size; i++) {
      df += sqrt(vocab[i].cn / static_cast<double>(b)) / dd;
      if (df > 1) df = 1;
      if (df > (a + 1) / static_cast<double>(class_size)) {
        vocab[i].class_index = a;
        if (a < class_size - 1) a++;
      } else {
        vocab[i].class_index = a;
      }
    }
  }

  // after dividing classes, update class start and class end information
  for (i = 0; i < vocab_size; i++)  {
    a = vocab[i].class_index;
    class_start[a] = min(i, class_start[a]);
    class_end[a] = max(i + 1, class_end[a]);
  }
  return 0;
}

int init_class() {
  // debug_mode = 1;
  debug_mode = 0;
  vocab_max_size = 100;  // largest length value for each word
  vocab_size = 0;
  vocab = (struct vocab_word *) calloc(vocab_max_size,
      sizeof(struct vocab_word));
  vocab_hash_size = 100000000;
  vocab_hash = reinterpret_cast<int *>(calloc(vocab_hash_size, sizeof(int)));
  old_classes = 1;

  // read vocab
  learnVocabFromTrainFile();

  // split classes
  splitClasses();

  return 0;
}

int create_data(const char *input_file, const char *output) {
  auto* store = singa::io::OpenStore("kvfile", output, singa::io::kCreate);
  WordRecord wordRecord;

  FILE *fin;
  int a, i;
  fin = fopen(input_file, "rb");

  int wcnt = 0;
  char key[BUFFER_LEN];
  char wordstr[MAX_STRING];
  string value;
  while (1) {
    readWord(wordstr, fin);
    if (feof(fin)) break;
    i = searchVocab(wordstr);
    if (i == -1) {
      if (debug_mode) printf("unknown word [%s] detected!", wordstr);
    } else {
      wordRecord.set_word(string(wordstr));
      wordRecord.set_word_index(i);
      int class_idx = vocab[i].class_index;
      wordRecord.set_class_index(class_idx);
      wordRecord.set_class_start(class_start[class_idx]);
      wordRecord.set_class_end(class_end[class_idx]);
      int length = snprintf(key, BUFFER_LEN, "%05d", wcnt++);
      wordRecord.SerializeToString(&value);
      store->Write(string(key, length), value);
    }
  }

  fclose(fin);
  store->Flush();
  delete store;
  return 0;
}

int argPos(char *str, int argc, char **argv) {
  int a;

  for (a = 1; a < argc; a++)
    if (!strcmp(str, argv[a]))
      return a;

  return -1;
}

int main(int argc, char **argv) {
  int i;
  FILE *f;

  // set debug mode
  i = argPos(const_cast<char *>("-debug"), argc, argv);
  if (i > 0) {
    debug_mode = 1;
    if (debug_mode > 0)
      printf("debug mode: %d\n", debug_mode);
  }

  // search for train file
  i = argPos(const_cast<char *>("-train"), argc, argv);
  if (i > 0) {
    if (i + 1 == argc) {
      printf("ERROR: training data file not specified!\n");
      return 0;
    }

    snprintf(train_file, strlen(argv[i + 1])+1, "%s", argv[i + 1]);

    if (debug_mode > 0)
      printf("train file: %s\n", train_file);

    f = fopen(train_file, "rb");
    if (f == NULL) {
      printf("ERROR: training data file not found!\n");
      return 0;
    }
    fclose(f);
  } else {
    printf("ERROR: training data must be set.\n");
  }

  // search for valid file
  i = argPos(const_cast<char *>("-valid"), argc, argv);
  if (i > 0) {
    if (i + 1 == argc) {
      printf("ERROR: validating data file not specified!\n");
      return 0;
    }

    snprintf(valid_file, strlen(argv[i + 1])+1, "%s", argv[i + 1]);

    if (debug_mode > 0)
      printf("valid file: %s\n", valid_file);

    f = fopen(valid_file, "rb");
    if (f == NULL) {
      printf("ERROR: validating data file not found!\n");
      return 0;
    }
    fclose(f);
    valid_mode = 1;
  }

  // search for test file
  i = argPos(const_cast<char *>("-test"), argc, argv);
  if (i > 0) {
    if (i + 1 == argc) {
      printf("ERROR: testing data file not specified!\n");
      return 0;
    }

    snprintf(test_file, strlen(argv[i + 1])+1, "%s", argv[i + 1]);

    if (debug_mode > 0)
      printf("test file: %s\n", test_file);

    f = fopen(test_file, "rb");
    if (f == NULL) {
      printf("ERROR: testing data file not found!\n");
      return 0;
    }
    fclose(f);
    test_mode = 1;
  }

  // search for class size
  i = argPos(const_cast<char *>("-class_size"), argc, argv);
  if (i > 0) {
    if (i + 1 == argc) {
      printf("ERROR: class size not specified!\n");
      return 0;
    }

    class_size = atoi(argv[i + 1]);

    if (debug_mode > 0)
      printf("class size: %d\n", class_size);
  }
  if (class_size <= 0) {
    printf("ERROR: no or invalid class size received!\n");
    return 0;
  }

  init_class();

  create_data(train_file, "train_data.bin");
  if (valid_mode) create_data(valid_file, "valid_data.bin");
  if (test_mode) create_data(test_file, "test_data.bin");

  return 0;
}
