// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "leveldb/env.h"
#include <stdio.h>
namespace leveldb {

Env::~Env() {
}

SequentialFile::~SequentialFile() {
}

RandomAccessFile::~RandomAccessFile() {
}

WritableFile::~WritableFile() {
}

Logger::~Logger() {
}

FileLock::~FileLock() {
}

void Log(Logger* info_log, const char* format, ...) {
  if (info_log != NULL) {
    va_list ap;
    va_start(ap, format);
    info_log->Logv(format, ap);
    va_end(ap);
  }
}

static Status DoWriteStringToFile(Env* env, const Slice& data,
                                  const std::string& fname,
                                  bool should_sync) {
  WritableFile* file;
  //printf(" 55555555555555i am DoWriteStringToFile,, before NewWritableFile, fname=%s\n",fname.c_str());
  Status s = env->NewWritableFile(fname, &file);
  //printf(" 55555555555555i am DoWriteStringToFile,, after NewWritableFile\n");
  if (!s.ok()) {
	  //printf("I am env.cc,DoWriteStringToFile,s.ok()!=0\n ");
    return s;
  }
   //printf(" 666666666666 am DoWriteStringToFile,, before Append\n");
  s = file->Append(data);
   //printf(" 6666666666666666 am DoWriteStringToFile,, after Append, s.ok()=%d\n",s.ok());
  if (s.ok() && should_sync) {
	   //printf(" 77777777777 am DoWriteStringToFile,Sync\n");
	  // exit(1);
    s = file->Sync();//just donothing
  }
  if (s.ok()) {
	   //printf(" I am DoWriteStringToFile,, b Close, s.ok()=%d\n",s.ok());
    s = file->Close();
  }
   //printf(" I am env.cc DoWriteStringToFile,, b delete, s.ok()=%d\n",s.ok());
  delete file;  // Will auto-close if we did not close above
   //printf(" I am env.cc DoWriteStringToFile,, a delete, s.ok()=%d\n",s.ok());
  if (!s.ok()) {
	  // printf("I am env.cc,DoWriteStringToFile,s.ok()!=0\n ");
	  // printf(" I am env.cc DoWriteStringToFile,, b DeleteFile, s.ok()=%d\n",s.ok());
    env->DeleteFile(fname);
	 //printf(" I am env.cc DoWriteStringToFile,, a DeleteFile, s.ok()=%d\n",s.ok());
  }
  
 // printf(" I am env.cc DoWriteStringToFile,s.ok()=%d\n",s.ok());
   
  return s;
}

Status WriteStringToFile(Env* env, const Slice& data,
                         const std::string& fname) {
  return DoWriteStringToFile(env, data, fname, false);
}

Status WriteStringToFileSync(Env* env, const Slice& data,
                             const std::string& fname) {
  return DoWriteStringToFile(env, data, fname, true);
}

Status ReadFileToString(Env* env, const std::string& fname, std::string* data) {
	//printf(" I am ReadFileToString\n");
	//printf(" I am ReadFileToString,dev=%p,fname=%s\n",env->env_flash->dev,fname.c_str());
	
  data->clear();
  SequentialFile* file;
  Status s = env->NewSequentialFile(fname, &file);
  
  //printf("----I am ReadFileToString, file=%p \n", file);
 
  if (!s.ok()) {
    return s;
  }
  static const int kBufferSize = 8192;
  char* space = new char[kBufferSize];
  while (true) {
    Slice fragment;
	///printf("----I am ReadFileToString \n");
	//printf("----I am ReadFileToString, file->file_addr=%p \n", file->file_addr);
	
	
    s = file->Read(kBufferSize, &fragment, space);//file contains the mmaped file blcok  address 
	//exit(1);
    if (!s.ok()) {
      break;
    }
    data->append(fragment.data(), fragment.size());
    if (fragment.empty()) {
      break;
    }
  }
  delete[] space;
  delete file;
  return s;
}

EnvWrapper::~EnvWrapper() {
}

}  // namespace leveldb
