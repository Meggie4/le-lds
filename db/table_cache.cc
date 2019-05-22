// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "db/table_cache.h"

#include "db/filename.h"
#include "leveldb/env.h"
#include "leveldb/table.h"
#include "util/coding.h"

#include <time.h>
#include <stdint.h>
#define s_to_ns 1000000000
extern double durations[];////durations[1]=Write_duration

namespace leveldb {

struct TableAndFile {
  RandomAccessFile* file;
  Table* table;
};

static void DeleteEntry(const Slice& key, void* value) {
  TableAndFile* tf = reinterpret_cast<TableAndFile*>(value);
  delete tf->table;
  delete tf->file;
  delete tf;
}

static void UnrefEntry(void* arg1, void* arg2) {
  Cache* cache = reinterpret_cast<Cache*>(arg1);
  Cache::Handle* h = reinterpret_cast<Cache::Handle*>(arg2);
  cache->Release(h);
}

TableCache::TableCache(const std::string& dbname,
                       const Options* options,
                       int entries)
    : env_(options->env),
      dbname_(dbname),
      options_(options),
      cache_(NewLRUCache(entries)) {
}

TableCache::~TableCache() {
  delete cache_;
}

int table_t;
Status TableCache::FindTable(uint64_t file_number, uint64_t file_size,
                             Cache::Handle** handle) {
							 
			//static int st=0;
			//st++;
			//if(table_t<10) printf("___________I am table_cache.cc, FindTable, begin \n" );
//printf("___________I am table_cache.cc, FindTable,  test segment fault, 111111111 \n" );
//printf("TableCache::FindTable. begin\n");


  Status s;
  char buf[sizeof(file_number)];
  EncodeFixed64(buf, file_number);
  Slice key(buf, sizeof(buf));
 

  *handle = cache_->Lookup(key);
	
  if (*handle == NULL) {// didn't find in cache, open from disk

    std::string fname = TableFileName(dbname_, file_number);
    RandomAccessFile* file = NULL;
    Table* table = NULL;
//if(!s.ok()) {printf("___________I am table_cache.cc, before NewRandomAccessFile, s.ok=%d \n",s.ok() ); }
//printf("TableCache::FindTable. 4444444\n");

    s = env_->NewRandomAccessFile(fname, &file);
	//printf("TableCache::FindTable. 555555555555555\n");

//if(!s.ok()) {printf("___________I am table_cache.cc, after NewRandomAccessFile, s.ok=%d ,fname=%s \n",s.ok(), fname.c_str() ); }
    if (!s.ok()) {
      std::string old_fname = SSTTableFileName(dbname_, file_number);
//printf("___________I am table_cache.cc, FindTable,  test segment fault, 5555555555 \n" );
      if (env_->NewRandomAccessFile(old_fname, &file).ok()) {
        s = Status::OK();
      }
//printf("___________I am table_cache.cc, FindTable,  test segment fault, 666666666666 \n" );
    }

//if(!s.ok()) {printf("___________I am table_cache.cc, before Table::Open, s.ok=%d \n",s.ok() ); }
    if (s.ok()) {
		//printf("in table_cache.cc, FindTable, Open, file_size=%d\n",file_size);
      s = Table::Open(*options_, file, file_size, &table);
    }


//if(!s.ok()) {printf("___________I am table_cache.cc, after Table::Open, s.ok=%d \n",s.ok()); }

    if (!s.ok()) {
      assert(table == NULL);
      delete file;
      // We do not cache error results so that if the error is transient,
      // or somebody repairs the file, we recover automatically.
    } else {
      TableAndFile* tf = new TableAndFile;
      tf->file = file;
      tf->table = table;
      *handle = cache_->Insert(key, tf, 1, &DeleteEntry);
    }
  }
  
	//if(!s.ok()) {
		//printf("___________I am table_cache.cc, FindTable, end s.ok=%d \n",s.ok() ); 
		//exit(1); 
	//}
	

//printf("___________I am table_cache.cc, FindTable,  test segment fault, 9999999999999 \n" );
//printf("TableCache::FindTable. end\n");
  return s;
}

Iterator* TableCache::NewIterator(const ReadOptions& options,
                                  uint64_t file_number,
                                  uint64_t file_size,
                                  Table** tableptr) {
  if (tableptr != NULL) {
    *tableptr = NULL;
  }

  Cache::Handle* handle = NULL;
  //printf("in table_cache.cc, NewIterator, file_size=%d\n",file_size);
  Status s = FindTable(file_number, file_size, &handle);
  if (!s.ok()) {
    return NewErrorIterator(s);
  }

  Table* table = reinterpret_cast<TableAndFile*>(cache_->Value(handle))->table;
  Iterator* result = table->NewIterator(options);
  result->RegisterCleanup(&UnrefEntry, cache_, handle);
  if (tableptr != NULL) {
    *tableptr = table;
  }
  return result;
}

Status TableCache::Get(const ReadOptions& options,
                       uint64_t file_number,
                       uint64_t file_size,
                       const Slice& k,
                       void* arg,
                       void (*saver)(void*, const Slice&, const Slice&)) {
//printf("___________I am table_cache.cc, Get,  test segment fault, 111111111 \n" );			
			//static int st=0;
			//st++;
			//table_t++;
			//if(st<5) printf("I am table_cache.cc, Get, begin \n" );

			
  Cache::Handle* handle = NULL;
 //  if(st<5) printf("*********I am table_cache.cc, Get, before FindTable\n" );
  Status s = FindTable(file_number, file_size, &handle);

 // if(st<5) printf("++++++++I am table_cache.cc, Get, after FindTable, s.ok()=%d \n" ,s.ok());
  if (s.ok()) {
//printf("___________I am table_cache.cc, Get,  test segment fault, 55555555555 \n" );	

    Table* t = reinterpret_cast<TableAndFile*>(cache_->Value(handle))->table;

//printf("___________I am table_cache.cc, Get,  test segment fault, 5.1111111 \n" );

		
    s = t->InternalGet(options, k, arg, saver);

//printf("___________I am table_cache.cc, Get,  test segment fault, 5.3333333 \n" );		
    cache_->Release(handle);
//printf("___________I am table_cache.cc, Get,  test segment fault, 666666666666666 \n" );		
  }
  
  
//printf("___________I am table_cache.cc, Get,  test segment fault, 999999999999999 \n" );		
  return s;
}

void TableCache::Evict(uint64_t file_number) {
  char buf[sizeof(file_number)];
  EncodeFixed64(buf, file_number);
  cache_->Erase(Slice(buf, sizeof(buf)));
}

}  // namespace leveldb
