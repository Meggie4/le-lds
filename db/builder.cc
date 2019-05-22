// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "db/builder.h"

#include "db/filename.h"
#include "db/dbformat.h"
#include "db/table_cache.h"
#include "db/version_edit.h"
#include "leveldb/db.h"
#include "leveldb/env.h"
#include "leveldb/iterator.h"

namespace leveldb {

Status BuildTable(const std::string& dbname,
                  Env* env,
                  const Options& options,
                  TableCache* table_cache,
                  Iterator* iter,
                  FileMetaData* meta) {  //caller is DBImpl::WriteLevevel0Table
	//printf(" ******** I am builder.cc ,BuildTable, begin \n ");
  Status s;
  meta->file_size = 0;
  iter->SeekToFirst();
	
	//printf("%d\n",meta->number);
	//printf("%d\n",meta->number);
  std::string fname = TableFileName(dbname, meta->number);
  
  //printf("in build.cc, fname=%s\n",fname.c_str());
  
  if (iter->Valid()) {
    WritableFile* file;
    s = env->NewWritableFile(fname, &file);
	//printf(" ******** I am builder.cc ,BuildTable, a NewWritableFile, s.ok()=%d \n ",s.ok());
    if (!s.ok()) {
      return s;
    }
//builder begin, so the meta.size is to be growed
    TableBuilder* builder = new TableBuilder(options, file);
    meta->smallest.DecodeFrom(iter->key());
    for (; iter->Valid(); iter->Next()) {
      Slice key = iter->key();
      meta->largest.DecodeFrom(key);
      builder->Add(key, iter->value());
    }

    // Finish and check for builder errors
    if (s.ok()) {
      s = builder->Finish();
	  //printf(" ******** I am builder.cc ,BuildTable, a builder->Finish, s.ok()=%d \n ",s.ok());
      if (s.ok()) {
        meta->file_size = builder->FileSize();
        assert(meta->file_size > 0);
      }
    } else {
      builder->Abandon();
    }
    delete builder;
	
  //builder finished, so now the meta.size has its value
  
    // Finish and check for file errors
    if (s.ok()) {
		// printf("in builder.cc,before sync, file_offset=%d\n", file->f_file->offset_bytes-4096);
		//file->test();
      s = file->Sync();
	  //file->test();
    }
    if (s.ok()) {
      s = file->Close();
    }
    delete file;
    //file = NULL;
 //printf(" ******** I am builder.cc ,BuildTable,xxxxxxx, s.ok()=%d \n ",s.ok());
    if (s.ok()) {
      // Verify that the table is usable
	   
      // Iterator* it = table_cache->NewIterator(ReadOptions(),meta->number,meta->file_size);
	
      // s = it->status();
	  
	  
      // delete it;
	   //printf(" ******** I am builder.cc ,BuildTable, a it->status, s.ok()=%d,file=%s \n ",s.ok(),fname.c_str());
    }
  }

  // Check for input iterator errors
  if (!iter->status().ok()) {
    s = iter->status();
  }

  if (s.ok() && meta->file_size > 0) {
    // Keep it
  } else {
    env->DeleteFile(fname);
  }
  return s;
}

}  // namespace leveldb
