// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "db/db_impl.h"

#include <algorithm>
#include <set>
#include <string>
#include <stdint.h>
#include <stdio.h>
#include <vector>
#include "db/builder.h"
#include "db/db_iter.h"
#include "db/dbformat.h"
#include "db/filename.h"
#include "db/log_reader.h"
#include "db/log_writer.h"
#include "db/memtable.h"
#include "db/table_cache.h"
#include "db/version_set.h"
#include "db/write_batch_internal.h"
#include "leveldb/db.h"
#include "leveldb/env.h"
#include "leveldb/status.h"
#include "leveldb/table.h"
#include "leveldb/table_builder.h"
#include "port/port.h"
#include "table/block.h"
#include "table/merger.h"
#include "table/two_level_iterator.h"
#include "util/coding.h"
#include "util/logging.h"
#include "util/mutexlock.h"


#include <time.h>
#include <stdint.h>
#define s_to_ns 1000000000
extern double durations[];////durations[1]=Write_duration
extern  int flash_using_exist;//0 is write 1 is read

namespace leveldb {

const int kNumNonTableCacheFiles = 10;

// Information kept for every waiting writer
struct DBImpl::Writer {
  Status status;
  WriteBatch* batch;
  bool sync;
  bool done;
  port::CondVar cv;

  explicit Writer(port::Mutex* mu) : cv(mu) { }
};

struct DBImpl::CompactionState {
  Compaction* const compaction;

  // Sequence numbers < smallest_snapshot are not significant since we
  // will never have to service a snapshot below smallest_snapshot.
  // Therefore if we have seen a sequence number S <= smallest_snapshot,
  // we can drop all entries for the same key with sequence numbers < S.
  SequenceNumber smallest_snapshot;

  // Files produced by compaction
  struct Output {
    uint64_t number;
    uint64_t file_size;
    InternalKey smallest, largest;
  };
  std::vector<Output> outputs;

  // State kept for output being generated
  WritableFile* outfile;
  TableBuilder* builder;

  uint64_t total_bytes;

  Output* current_output() { return &outputs[outputs.size()-1]; }

  explicit CompactionState(Compaction* c)
      : compaction(c),
        outfile(NULL),
        builder(NULL),
        total_bytes(0) {
  }
};

// Fix user-supplied options to be reasonable
template <class T,class V>
static void ClipToRange(T* ptr, V minvalue, V maxvalue) {
  if (static_cast<V>(*ptr) > maxvalue) *ptr = maxvalue;
  if (static_cast<V>(*ptr) < minvalue) *ptr = minvalue;
}
Options SanitizeOptions(const std::string& dbname,
                        const InternalKeyComparator* icmp,
                        const InternalFilterPolicy* ipolicy,
                        const Options& src) {
printf("dbimpl, SanitizeOptions, begin\n");
  Options result = src;
  result.comparator = icmp;
  result.filter_policy = (src.filter_policy != NULL) ? ipolicy : NULL;
  ClipToRange(&result.max_open_files,    64 + kNumNonTableCacheFiles, 50000);
  ClipToRange(&result.write_buffer_size, 64<<10,                      1<<30);
  ClipToRange(&result.block_size,        1<<10,                       4<<20);
  if (result.info_log == NULL) {
    // Open a log file in the same directory as the db
    src.env->CreateDir(dbname);  // In case it does not exist //this must be rerurned status::OK(), or will segment fault
printf("dbimpl, SanitizeOptions, 111111111111\n"); 
 
    src.env->RenameFile(InfoLogFileName(dbname), OldInfoLogFileName(dbname));//but this does not ned return OK.
printf("dbimpl, SanitizeOptions, 2222222\n");

    Status s = src.env->NewLogger(InfoLogFileName(dbname), &result.info_log);
printf("dbimpl, SanitizeOptions, 333333333\n");

    if (!s.ok()) {
      // No place suitable for logging
      result.info_log = NULL;
    }
  }
  if (result.block_cache == NULL) {
    result.block_cache = NewLRUCache(8 << 20);
  }
printf("dbimpl, SanitizeOptions, end\n");

  return result;
}

DBImpl::DBImpl(const Options& raw_options, const std::string& dbname)
    : env_(raw_options.env),
      internal_comparator_(raw_options.comparator),
      internal_filter_policy_(raw_options.filter_policy),
      options_(SanitizeOptions(dbname, &internal_comparator_,
                               &internal_filter_policy_, raw_options)),
      owns_info_log_(options_.info_log != raw_options.info_log),
      owns_cache_(options_.block_cache != raw_options.block_cache),
      dbname_(dbname),
      db_lock_(NULL),
      shutting_down_(NULL),
      bg_cv_(&mutex_),
      mem_(new MemTable(internal_comparator_)),
      imm_(NULL),
      logfile_(NULL),
      logfile_number_(0),
      log_(NULL),
      seed_(0),
      tmp_batch_(new WriteBatch),
      bg_compaction_scheduled_(false),
      manual_compaction_(NULL) {
printf("dbimpl.cc, DBImpl, 1111\n" );
  mem_->Ref();
  has_imm_.Release_Store(NULL);

  // Reserve ten files or so for other uses and give the rest to TableCache.
  const int table_cache_size = options_.max_open_files - kNumNonTableCacheFiles;
  table_cache_ = new TableCache(dbname_, &options_, table_cache_size);

  versions_ = new VersionSet(dbname_, &options_, table_cache_,
                             &internal_comparator_);
}

DBImpl::~DBImpl() {
  // Wait for background work to finish
  mutex_.Lock();
  shutting_down_.Release_Store(this);  // Any non-NULL value is ok
  while (bg_compaction_scheduled_) {
    bg_cv_.Wait();
  }
  mutex_.Unlock();

  if (db_lock_ != NULL) {
    env_->UnlockFile(db_lock_);
  }

  delete versions_;
  if (mem_ != NULL) mem_->Unref();
  if (imm_ != NULL) imm_->Unref();
  delete tmp_batch_;
  delete log_;
  delete logfile_;
  delete table_cache_;

  if (owns_info_log_) {
    delete options_.info_log;
  }
  if (owns_cache_) {
    delete options_.block_cache;
  }
}

Status DBImpl::NewDB() {
	
	printf(" ++++++++++++++++++hello, I am DBImpl::NewDB begin\n");
	
	//exit(1);
  VersionEdit new_db;
  new_db.SetComparatorName(user_comparator()->Name());
  new_db.SetLogNumber(0);
  new_db.SetNextFile(2);
  new_db.SetLastSequence(0);

  const std::string manifest = DescriptorFileName(dbname_, 1);
  WritableFile* file;
 	 //  printf(" __ hello, I am DBImpl::NewDB. Here\n");

  Status s = env_->NewWritableFile(manifest, &file);//this will create MANIFEST file. Create an empty file. We acess the Falsh_file structure from this file
  
 printf(" __ hello, I am DBImpl::NewDB.after NewWritableFile \n");
  if (!s.ok()) {
    return s;
  }
  {
	 printf(" __ hello, I am DBImpl::NewDB.before log(file) \n");
    log::Writer log(file);//package the manifest file to be a log file.
	printf(" __ hello, I am DBImpl::NewDB.after log(file) \n");
    std::string record;
    new_db.EncodeTo(&record);
	printf(" __ hello, I am DBImpl::NewDB.after EncodeTo record=%s \n",record.c_str());
    s = log.AddRecord(record);//wrtie the string to Manifest segment. Because log is constructed using the manifest Flash_file
	printf(" __ hello, I am DBImpl::NewDB.after AddRecord \n");
    if (s.ok()) {
		//printf(" __ hello, I am DBImpl::NewDB. before close \n");
      s = file->Close();//do nothing 
	 // printf(" __ hello, I am DBImpl::NewDB. after close \n");
    }
  }
  //printf(" __ hello, I am DBImpl::NewDB. before delete \n");
  
  //delete file;
  //printf("8 hello, I am DBImpl::NewDB. after delete s.ok=%d \n",s.ok());
  if (s.ok()) {
    // Make "CURRENT" file that points to the new manifest file.
	printf("99999999999999999999999 hello, I am DBImpl::NewDB. before SetCurrentFile \n");
    s = SetCurrentFile(env_, dbname_, 1);//this may create the CURRENT file. Function in filename.cc
	
  } else {
	  printf("66666666666666666666666 hello, I am DBImpl::NewDB. before DeleteFile \n");
    env_->DeleteFile(manifest);//this delete the file
  }
  //printf(" +++++++++++++++++=hello, I am DBImpl::NewDB end\n");
  return s;
}

void DBImpl::MaybeIgnoreError(Status* s) const {
  if (s->ok() || options_.paranoid_checks) {
    // No change needed
  } else {
    Log(options_.info_log, "Ignoring error %s", s->ToString().c_str());
    *s = Status::OK();
  }
}

void DBImpl::DeleteObsoleteFiles() {
	///printf("dBImpl.cc, DeleteObsoleteFiles,11111, return\n");
	return;

struct timespec begin_delete, end_delete;
clock_gettime(CLOCK_MONOTONIC,&begin_delete);

  if (!bg_error_.ok()) {
    // After a background error, we don't know whether a new version may
    // or may not have been committed, so we cannot safely garbage collect.
    return;
  }

  // Make a set of all of the live files
  std::set<uint64_t> live = pending_outputs_;
  versions_->AddLiveFiles(&live);

  std::vector<std::string> filenames;
  env_->GetChildren(dbname_, &filenames); // Ignoring errors on purpose
  
  uint64_t number;
  FileType type;
 //printf("DeleteObsoleteFiles,oooo,filenames.size()=%d\n",filenames.size());
  for (size_t i = 0; i < filenames.size(); i++) {
  //printf("DeleteObsoleteFiles,iiiiiiiiii,filenames.size()=%d, fileI=%s\n",filenames.size(),filenames[i].c_str());
    if (ParseFileName(filenames[i], &number, &type)) {
	//printf("DeleteObsoleteFiles,3333333333333\n");
      bool keep = true;
      switch (type) {
        case kLogFile:
			//printf("dn_impl.cc,kLogFile, filenames[i]=%s,  versions_->LogNumber()=%d\n",filenames[i].c_str(), versions_->LogNumber());
          keep = ((number >= versions_->LogNumber()) ||
                  (number == versions_->PrevLogNumber()));
          break;
        case kDescriptorFile:
		//printf("dn_impl.cc,kDescriptorFile, filenames[i]=%s,  versions_->ManifestFileNumber()=%d\n",filenames[i].c_str(), versions_->ManifestFileNumber());
          // Keep my manifest file, and any newer incarnations'
          // (in case there is a race that allows other incarnations)
          keep = (number >= versions_->ManifestFileNumber());
          break;
        case kTableFile:
          keep = (live.find(number) != live.end());
			//printf("dn_impl.cc,kDescriptorFile, filenames[i]=%s,  keep=%d\n",filenames[i].c_str(),keep);
			//printf("dn_impl.cc,live.find(number)=%d,live.end()=%d\n",live.find(number),live.end());
          break;
        case kTempFile:
          // Any temp files that are currently being written to must
          // be recorded in pending_outputs_, which is inserted into "live"
          keep = (live.find(number) != live.end());
          break;
        case kCurrentFile:
        case kDBLockFile:
        case kInfoLogFile:
          keep = true;
          break;
      }
	//printf("DeleteObsoleteFiles,kkkkkkk,keep=%d\n",keep);
	//exit(9);
      if (!keep) {
        if (type == kTableFile) {
          table_cache_->Evict(number);
        }
        Log(options_.info_log, "Delete type=%d #%lld\n",
            int(type),
            static_cast<unsigned long long>(number));
		//printf("in dn_impl.cc, xxxx,33333\n");
        env_->DeleteFile(dbname_ + "/" + filenames[i]);
      }
    }
  }
  
  
clock_gettime(CLOCK_MONOTONIC,&end_delete);
durations[60] +=( (int)end_delete.tv_sec +((double)end_delete.tv_nsec)/s_to_ns ) - ( (int)begin_delete.tv_sec+((double)begin_delete.tv_nsec)/s_to_ns );



}

Status DBImpl::Recover(VersionEdit* edit) {
 printf("<<<<<<<<<<<<<<<<<<<<<<<<<<i am DBImpl::Recover, begin\n ");
	Status s;
	
  mutex_.AssertHeld();

  // Ignore error from CreateDir since the creation of the DB is
  // committed only when the descriptor is created, and this directory
  // may already exist from a previous failed creation attempt.
  
  
  env_->CreateDir(dbname_);//good, do the formation work
  assert(db_lock_ == NULL);
  s = env_->LockFile(LockFileName(dbname_), &db_lock_);//good, I just make it do nothing
  if (!s.ok()) {
    return s;
  }

 printf("<<<<<<<<<<<<<<<<<<<<<<<<<<i am DBImpl::Recover, dbname_=%s\n", dbname_.c_str());
  if (!env_->FileExists(CurrentFileName(dbname_))) {// current file indicates the latest manifest, LDS does need it.
	  printf("<<<<<<<<<<<<<<<<<<<<<<<<<<i am DBImpl::Recover, Current file not exist\n ");
	  
    if (options_.create_if_missing) {
		printf(">>>>>>>>>>>>>>>>>>>>>>>>>>>>. i am DBImpl::Recover,  before NewDB()\n ");

      s = NewDB();//after this, 2 files will be created, CURRENT  MANIFEST-000001. In two function up 
	  printf("<<<<<<<<<<<<<<<<<<<<<<<<<<i am DBImpl::Recover, after NewDB(),s.ok()=%d\n ",s.ok());
      if (!s.ok()) {
        return s;
      }
    } else {
      return Status::InvalidArgument(
          dbname_, "does not exist (create_if_missing is false)");
    }
  } else {
    if (options_.error_if_exists) {
      return Status::InvalidArgument(
          dbname_, "exists (error_if_exists is true)");
    }
  }
 printf("_____Hello, I am DBImpl::Recover ,before versions_->Recover\n");
// exit(1);
  s = versions_->Recover();  //this will delete MANIFEST-000001
  // exit(1);
   printf("_____Hello, I am DBImpl::Recover ,after versions_->Recover, s.ok()=%d\n",s.ok());
  if (s.ok()) {
    SequenceNumber max_sequence(0);

    // Recover from all newer log files than the ones named in the
    // descriptor (new log files may have been added by the previous
    // incarnation without registering them in the descriptor).
    //
    // Note that PrevLogNumber() is no longer used, but we pay
    // attention to it in case we are recovering a database
    // produced by an older version of leveldb.
    const uint64_t min_log = versions_->LogNumber();
    const uint64_t prev_log = versions_->PrevLogNumber();
    std::vector<std::string> filenames;
    s = env_->GetChildren(dbname_, &filenames); 
	
	   //printf("_____Hello, I am DBImpl::Recover ,44444 s.ok()=%d, filenames.size=%d \n",s.ok(), filenames.size());
	   int i;
	   //for(i=0;i<filenames.size();i++){
		  /// printf("%s ",filenames.at(i).c_str());
	  // }
	   printf("\n");
	 
    if (!s.ok()) {
      return s;
    }
    std::set<uint64_t> expected;
    versions_->AddLiveFiles(&expected);
	printf("DBImpl::Recover, expected.size=%d\n",expected.size());
	 // for (std::set<uint64_t>::iterator it=expected.begin(); it!=expected.end(); ++it){
		// printf("%d ",*it);
	 // }
	 //printf("\n");
	 //exit(9);
    uint64_t number;
    FileType type;
    std::vector<uint64_t> logs;
	
    for (size_t i = 0; i < filenames.size(); i++) {
      if (ParseFileName(filenames[i], &number, &type)) {
        expected.erase(number);
        if (type == kLogFile && ((number >= min_log) || (number == prev_log)))
          logs.push_back(number);
      }
    }
	
	expected.clear();//I insert this to force the continuing
	
    if (!expected.empty()) {
      char buf[50];
      snprintf(buf, sizeof(buf), "%d missing files; e.g.",
               static_cast<int>(expected.size()));
      return Status::Corruption(buf, TableFileName(dbname_, *(expected.begin())));
    }

    // Recover in the order in which the logs were generated
	printf("DBImpl::Recover, logs.size=%d\n",logs.size());
	//exit(1);
    std::sort(logs.begin(), logs.end());
    for (size_t i = 0; i < logs.size(); i++) {
      //s = RecoverLogFile(logs[i], edit, &max_sequence);
//printf("_____Hello, I am DBImpl::Recover ,after RecoverLogFile, s.ok()=%d\n",s.ok());
      // The previous incarnation may not have written any MANIFEST
      // records after allocating this log number.  So we manually
      // update the file number allocation counter in VersionSet.
	  
      //versions_->MarkFileNumberUsed(logs[i]);
    }

    if (s.ok()) {
      if (versions_->LastSequence() < max_sequence) {
        versions_->SetLastSequence(max_sequence);
      }
    }
  }
 printf("<<<<<<<<<<<<<<<<<<<<<<<<<<i am DBImpl::Recover,end, s.ok()=%d\n",s.ok());
 //exit(1);
  return s;
}

Status DBImpl::RecoverLogFile(uint64_t log_number,
                              VersionEdit* edit,
                              SequenceNumber* max_sequence) {
  struct LogReporter : public log::Reader::Reporter {
    Env* env;
    Logger* info_log;
    const char* fname;
    Status* status;  // NULL if options_.paranoid_checks==false
    virtual void Corruption(size_t bytes, const Status& s) {
      Log(info_log, "%s%s: dropping %d bytes; %s",
          (this->status == NULL ? "(ignoring error) " : ""),
          fname, static_cast<int>(bytes), s.ToString().c_str());
      if (this->status != NULL && this->status->ok()) *this->status = s;
    }
  };

  mutex_.AssertHeld();

  // Open the log file
  std::string fname = LogFileName(dbname_, log_number);
  SequentialFile* file;
  Status status = env_->NewSequentialFile(fname, &file);
  if (!status.ok()) {
    MaybeIgnoreError(&status);
    return status;
  }

  // Create the log reader.
  LogReporter reporter;
  reporter.env = env_;
  reporter.info_log = options_.info_log;
  reporter.fname = fname.c_str();
  reporter.status = (options_.paranoid_checks ? &status : NULL);
  // We intentionally make log::Reader do checksumming even if
  // paranoid_checks==false so that corruptions cause entire commits
  // to be skipped instead of propagating bad information (like overly
  // large sequence numbers).
  log::Reader reader(file, &reporter, true/*checksum*/,
                     0/*initial_offset*/);
  Log(options_.info_log, "Recovering log #%llu",
      (unsigned long long) log_number);

  // Read all the records and add to a memtable
  std::string scratch;
  Slice record;
  WriteBatch batch;
  MemTable* mem = NULL;
  while (reader.ReadRecord(&record, &scratch) &&
         status.ok()) {
    if (record.size() < 12) {
      reporter.Corruption(
          record.size(), Status::Corruption("log record too small"));
      continue;
    }
    WriteBatchInternal::SetContents(&batch, record);

    if (mem == NULL) {
      mem = new MemTable(internal_comparator_);
      mem->Ref();
    }
    status = WriteBatchInternal::InsertInto(&batch, mem);
    MaybeIgnoreError(&status);
    if (!status.ok()) {
      break;
    }
    const SequenceNumber last_seq =
        WriteBatchInternal::Sequence(&batch) +
        WriteBatchInternal::Count(&batch) - 1;
    if (last_seq > *max_sequence) {
      *max_sequence = last_seq;
    }

    if (mem->ApproximateMemoryUsage() > options_.write_buffer_size) {
      status = WriteLevel0Table(mem, edit, NULL);
      if (!status.ok()) {
        // Reflect errors immediately so that conditions like full
        // file-systems cause the DB::Open() to fail.
        break;
      }
      mem->Unref();
      mem = NULL;
    }
  }

  if (status.ok() && mem != NULL) {
    status = WriteLevel0Table(mem, edit, NULL);
    // Reflect errors immediately so that conditions like full
    // file-systems cause the DB::Open() to fail.
  }

  if (mem != NULL) mem->Unref();
  delete file;
  return status;
}

Status DBImpl::WriteLevel0Table(MemTable* mem, VersionEdit* edit,
                                Version* base) {
	//printf(" =========I am db_impl.cc, WriteLevel0Table , begin, \n");
		//exit(9);
  mutex_.AssertHeld();
  const uint64_t start_micros = env_->NowMicros();
  FileMetaData meta;
  meta.number = versions_->NewFileNumber();
  pending_outputs_.insert(meta.number);
  Iterator* iter = mem->NewIterator();
  Log(options_.info_log, "Level-0 table #%llu: started",
      (unsigned long long) meta.number);

  Status s;
  {
    mutex_.Unlock();
	//printf(" =========I am db_impl.cc, WriteLevel0Table, meta.number=%d\n",meta.number);
	//exit(9);
	//printf("DBImpl::WriteLevel0Table, 0000\n");
		//printf(" =========I am db_impl.cc, WriteLevel0Table , b BuildTable 11 , s.ok()=%d\n",s.ok());

    s = BuildTable(dbname_, env_, options_, table_cache_, iter, &meta);
	//printf(" =========I am db_impl.cc, WriteLevel0Table , a BuildTable 11 , s.ok()=%d\n",s.ok());

	//printf("DBImpl::WriteLevel0Table, 1111111111\n");
    mutex_.Lock();
  }
//printf(" =========I am db_impl.cc, WriteLevel0Table , a BuildTable , s.ok()=%d\n",s.ok());
  Log(options_.info_log, "Level-0 table #%llu: %lld bytes %s",
      (unsigned long long) meta.number,
      (unsigned long long) meta.file_size,
      s.ToString().c_str());
  delete iter;
  pending_outputs_.erase(meta.number);


  // Note that if file_size is zero, the file has been deleted and
  // should not be added to the manifest.
  int level = 0;
  if (s.ok() && meta.file_size > 0) {
    const Slice min_user_key = meta.smallest.user_key();
    const Slice max_user_key = meta.largest.user_key();
    if (base != NULL) {
      level = base->PickLevelForMemTableOutput(min_user_key, max_user_key);
    }
    edit->AddFile(level, meta.number, meta.file_size,
                  meta.smallest, meta.largest);
  }

  CompactionStats stats;
  stats.micros = env_->NowMicros() - start_micros;
  stats.bytes_written = meta.file_size;
  stats_[level].Add(stats);
  //printf(" =========I am db_impl.cc, WriteLevel0Table , end, \n");
  return s;
}

void DBImpl::CompactMemTable() {
	//printf(" _________Iam db_impl.cc ,CompactMemTable ,begin, bg_error_.ok()=%d, imm_=%p \n",bg_error_.ok(),imm_);
	
  mutex_.AssertHeld();
  assert(imm_ != NULL);

  // Save the contents of the memtable as a new Table
  VersionEdit edit;
  Version* base = versions_->current();
  base->Ref();
 //printf(" _________Iam db_impl.cc ,CompactMemTable ,xxxxx\n");
//printf(" _________Iam db_impl.cc ,CompactMemTable ,b  WriteLevel0Table, s.ok()=%d, imm_=%p \n",s.ok(),imm_);
 
  Status s = WriteLevel0Table(imm_, &edit, base);
  base->Unref();
//printf(" _________Iam db_impl.cc ,CompactMemTable ,a  WriteLevel0Table, s.ok()=%d, imm_=%p \n",s.ok(),imm_);
  if (s.ok() && shutting_down_.Acquire_Load()) {
    s = Status::IOError("Deleting DB during memtable compaction");
  }

  // Replace immutable memtable with the generated Table
  if (s.ok()) {
    edit.SetPrevLogNumber(0);
    edit.SetLogNumber(logfile_number_);  // Earlier logs no longer needed
    s = versions_->LogAndApply(&edit, &mutex_);
  }
//printf(" _________Iam db_impl.cc ,CompactMemTable ,b  Commit to the new state, s.ok()=%d, imm_=%p \n",s.ok(),imm_);
  if (s.ok()) {
    // Commit to the new state
    imm_->Unref();
    imm_ = NULL;
    has_imm_.Release_Store(NULL);
	//printf("ddddddd,22222\n");
    DeleteObsoleteFiles();
  } else {
	  //printf(" _________Iam db_impl.cc ,CompactMemTable ,a  Commit to the new state, s.ok()=%d, imm_=%p \n",s.ok(),imm_);
	  if( s.ok()==0) printf("_____I am db_impl.cc, CompactMemTable   s.ok()=%d\n", s.ok());
    RecordBackgroundError(s);
  }
 //printf("_____I am db_impl.cc, CompactMemTable  end\n" );
}

void DBImpl::CompactRange(const Slice* begin, const Slice* end) {
printf("dbimpl,DBImpl::CompactRange begin\n");
  int max_level_with_files = 1;
  {
    MutexLock l(&mutex_);
    Version* base = versions_->current();
    for (int level = 1; level < config::kNumLevels; level++) {
      if (base->OverlapInLevel(level, begin, end)) {
        max_level_with_files = level;
      }
    }
  }
  TEST_CompactMemTable(); // TODO(sanjay): Skip if memtable does not overlap
  for (int level = 0; level < max_level_with_files; level++) {
    TEST_CompactRange(level, begin, end);
  }
  printf("dbimpl,DBImpl::CompactRange end\n");

}

void DBImpl::TEST_CompactRange(int level, const Slice* begin,const Slice* end) {
  assert(level >= 0);
  assert(level + 1 < config::kNumLevels);

  InternalKey begin_storage, end_storage;

  ManualCompaction manual;
  manual.level = level;
  manual.done = false;
  if (begin == NULL) {
    manual.begin = NULL;
  } else {
    begin_storage = InternalKey(*begin, kMaxSequenceNumber, kValueTypeForSeek);
    manual.begin = &begin_storage;
  }
  if (end == NULL) {
    manual.end = NULL;
  } else {
    end_storage = InternalKey(*end, 0, static_cast<ValueType>(0));
    manual.end = &end_storage;
  }

  MutexLock l(&mutex_);
  while (!manual.done && !shutting_down_.Acquire_Load() && bg_error_.ok()) {
    if (manual_compaction_ == NULL) {  // Idle
      manual_compaction_ = &manual;
      MaybeScheduleCompaction();
    } else {  // Running either my compaction or another compaction.
      bg_cv_.Wait();
    }
  }
  if (manual_compaction_ == &manual) {
    // Cancel my manual compaction since we aborted early for some reason.
    manual_compaction_ = NULL;
  }
}

Status DBImpl::TEST_CompactMemTable() {
  // NULL batch means just wait for earlier writes to be done
  Status s = Write(WriteOptions(), NULL);
  if (s.ok()) {
    // Wait until the compaction completes
    MutexLock l(&mutex_);
    while (imm_ != NULL && bg_error_.ok()) {
      bg_cv_.Wait();
    }
    if (imm_ != NULL) {
      s = bg_error_;
    }
  }
  return s;
}

void DBImpl::RecordBackgroundError(const Status& s) {
  mutex_.AssertHeld();
  printf("_____I am db_impl.cc, RecordBackgroundError   bg_error_.ok()=%d\n", bg_error_.ok());
  if (bg_error_.ok()) {
    bg_error_ = s;
	//if( bg_error_.ok()==0) printf("_____I am db_impl.cc, RecordBackgroundError   bg_error_.ok()=%d\n", bg_error_.ok());
    bg_cv_.SignalAll();
  }
}

void DBImpl::MaybeScheduleCompaction() {
	//printf(" Iam db_impl.cc ,MaybeScheduleCompaction ,begin, bg_error_.ok()=%d \n",bg_error_.ok());
  mutex_.AssertHeld();
  if (bg_compaction_scheduled_) {
    // Already scheduled
  } else if (shutting_down_.Acquire_Load()) {
    // DB is being deleted; no more background compactions
  } else if (!bg_error_.ok()) {
    // Already got an error; no more changes
  } else if (imm_ == NULL &&
             manual_compaction_ == NULL &&
             !versions_->NeedsCompaction()) {
    // No work to be done
  } else {
	//printf(" Iam db_impl.cc ,MaybeScheduleCompaction, env_->Schedule\n");
    bg_compaction_scheduled_ = true;
    env_->Schedule(&DBImpl::BGWork, this);//this
  }
}

void DBImpl::BGWork(void* db) {
	
  reinterpret_cast<DBImpl*>(db)->BackgroundCall();
}

void DBImpl::BackgroundCall() {
	//printf(" Iam db_impl.cc ,BackgroundCall ,begin, bg_error_.ok()=%d \n",bg_error_.ok());
struct timespec begin_bgc, end_bgc;
clock_gettime(CLOCK_MONOTONIC,&begin_bgc);
  MutexLock l(&mutex_);
  assert(bg_compaction_scheduled_);
  if (shutting_down_.Acquire_Load()) {
    // No more background work when shutting down.
  } else if (!bg_error_.ok()) {
    // No more background work after a background error.
  } else {
    BackgroundCompaction();
  }

  bg_compaction_scheduled_ = false;

  // Previous compaction may have produced too many files in a level,
  // so reschedule another compaction if needed.
  MaybeScheduleCompaction();
  bg_cv_.SignalAll();
clock_gettime(CLOCK_MONOTONIC,&end_bgc);
durations[43] +=( (int)end_bgc.tv_sec +((double)end_bgc.tv_nsec)/s_to_ns ) - ( (int)begin_bgc.tv_sec+((double)begin_bgc.tv_nsec)/s_to_ns );
  	//printf(" Iam db_impl.cc ,BackgroundCall ,end, bg_error_.ok()=%d \n",bg_error_.ok());

}

void DBImpl::BackgroundCompaction() {

//struct timespec begin_blank, end_blank;
//clock_gettime(CLOCK_MONOTONIC,&begin_blank);

   
	 
	//printf(" Iam db_impl.cc ,BackgroundCompaction ,begin, bg_error_.ok()=%d \n",bg_error_.ok());
  mutex_.AssertHeld();

  if (imm_ != NULL) {
	  //printf(" Iam db_impl.cc ,BackgroundCompaction ,imm is not null. here return\n");
//struct timespec begin_CompactMemTable, end_CompactMemTable;
//clock_gettime(CLOCK_MONOTONIC,&begin_CompactMemTable);
    CompactMemTable();
//clock_gettime(CLOCK_MONOTONIC,&end_CompactMemTable);
//durations[18] +=( (int)end_CompactMemTable.tv_sec +((double)end_CompactMemTable.tv_nsec)/s_to_ns ) - ( (int)begin_CompactMemTable.tv_sec+((double)begin_CompactMemTable.tv_nsec)/s_to_ns );

    return;
  }

  Compaction* c;
  bool is_manual = (manual_compaction_ != NULL);
  InternalKey manual_end;
  if (is_manual) {
    ManualCompaction* m = manual_compaction_;
	//printf(" Iam db_impl.cc ,BackgroundCompaction ,in is_manual, m->level=%d \n",m->level);
    c = versions_->CompactRange(m->level, m->begin, m->end);
    m->done = (c == NULL);
    if (c != NULL) {
      manual_end = c->input(0, c->num_input_files(0) - 1)->largest;
    }
    Log(options_.info_log,
        "Manual compaction at level-%d from %s .. %s; will stop at %s\n",
        m->level,
        (m->begin ? m->begin->DebugString().c_str() : "(begin)"),
        (m->end ? m->end->DebugString().c_str() : "(end)"),
        (m->done ? "(end)" : manual_end.DebugString().c_str()));
  } else {
	  //printf(" Iam db_impl.cc ,BackgroundCompaction ,in not is_manual,\n");
    c = versions_->PickCompaction();
  }

  Status status;
  if (c == NULL) {
    // Nothing to do
  } else if (!is_manual && c->IsTrivialMove()) {
    // Move file to next level
    assert(c->num_input_files(0) == 1);
    FileMetaData* f = c->input(0, 0);
	//printf("xxxx,44444\n");
    c->edit()->DeleteFile(c->level(), f->number);
    c->edit()->AddFile(c->level() + 1, f->number, f->file_size,
                       f->smallest, f->largest);
    status = versions_->LogAndApply(c->edit(), &mutex_);
    if (!status.ok()) {
		if( status.ok()==0) printf("_____I am db_impl.cc, BackgroundCompaction  a LogAndApply  status.ok()=%d\n", status.ok());
      RecordBackgroundError(status);
    }
    VersionSet::LevelSummaryStorage tmp;
    Log(options_.info_log, "Moved #%lld to level-%d %lld bytes %s: %s\n",
        static_cast<unsigned long long>(f->number),
        c->level() + 1,
        static_cast<unsigned long long>(f->file_size),
        status.ToString().c_str(),
        versions_->LevelSummary(&tmp));
  } else {
    CompactionState* compact = new CompactionState(c);
	
    
	//struct timespec begin_DoCompactionWork, end_DoCompactionWork;
//clock_gettime(CLOCK_MONOTONIC,&begin_DoCompactionWork);
    status = DoCompactionWork(compact);//line 857
//clock_gettime(CLOCK_MONOTONIC,&end_DoCompactionWork);
//durations[19] +=( (int)end_DoCompactionWork.tv_sec +((double)end_DoCompactionWork.tv_nsec)/s_to_ns ) - ( (int)begin_DoCompactionWork.tv_sec+((double)begin_DoCompactionWork.tv_nsec)/s_to_ns );

    if (!status.ok()) {
		//if( status.ok()==0) printf("_____I am db_impl.cc, BackgroundCompaction , a DoCompactionWork  status.ok()=%d\n", status.ok());
      RecordBackgroundError(status);
    }
    CleanupCompaction(compact);
    c->ReleaseInputs();
	//printf("ddddddd,333333\n");
    DeleteObsoleteFiles();
  }
  delete c;

  if (status.ok()) {
    // Done
  } else if (shutting_down_.Acquire_Load()) {
    // Ignore compaction errors found during shutting down
  } else {
    Log(options_.info_log,
        "Compaction error: %s", status.ToString().c_str());
  }

  if (is_manual) {
    ManualCompaction* m = manual_compaction_;
    if (!status.ok()) {
      m->done = true;
    }
    if (!m->done) {
      // We only compacted part of the requested range.  Update *m
      // to the range that is left to be compacted.
      m->tmp_storage = manual_end;
      m->begin = &m->tmp_storage;
    }
    manual_compaction_ = NULL;
  }
  
   //clock_gettime(CLOCK_MONOTONIC,&end_blank);
//durations[17] +=( (int)end_blank.tv_sec +((double)end_blank.tv_nsec)/s_to_ns ) - ( (int)begin_blank.tv_sec+((double)begin_blank.tv_nsec)/s_to_ns );

}

void DBImpl::CleanupCompaction(CompactionState* compact) {
  mutex_.AssertHeld();
  if (compact->builder != NULL) {
    // May happen if we get a shutdown call in the middle of compaction
    compact->builder->Abandon();
    delete compact->builder;
  } else {
    assert(compact->outfile == NULL);
  }
  delete compact->outfile;
  for (size_t i = 0; i < compact->outputs.size(); i++) {
    const CompactionState::Output& out = compact->outputs[i];
    pending_outputs_.erase(out.number);
  }
  delete compact;
}

Status DBImpl::OpenCompactionOutputFile(CompactionState* compact) {
  assert(compact != NULL);
  assert(compact->builder == NULL);
  uint64_t file_number;
   //struct timespec begin_OpenCompactionOutputFile1, end_OpenCompactionOutputFile1;
//clock_gettime(CLOCK_MONOTONIC,&begin_OpenCompactionOutputFile1);
	  
  
  {
    mutex_.Lock();
    file_number = versions_->NewFileNumber();
    pending_outputs_.insert(file_number);
    CompactionState::Output out;
    out.number = file_number;
    out.smallest.Clear();
    out.largest.Clear();
    compact->outputs.push_back(out);
    mutex_.Unlock();
  }

//clock_gettime(CLOCK_MONOTONIC,&end_OpenCompactionOutputFile1);
//durations[25] +=( (int)end_OpenCompactionOutputFile1.tv_sec +((double)end_OpenCompactionOutputFile1.tv_nsec)/s_to_ns ) - ( (int)begin_OpenCompactionOutputFile1.tv_sec+((double)begin_OpenCompactionOutputFile1.tv_nsec)/s_to_ns );

// Make the output file
  std::string fname = TableFileName(dbname_, file_number);
  //struct timespec begin_OpenCompactionOutputFile2, end_OpenCompactionOutputFile2;
//clock_gettime(CLOCK_MONOTONIC,&begin_OpenCompactionOutputFile2);	  
 	
  
  Status s = env_->NewWritableFile(fname, &compact->outfile);
 
//clock_gettime(CLOCK_MONOTONIC,&end_OpenCompactionOutputFile2);
//durations[26] +=( (int)end_OpenCompactionOutputFile2.tv_sec +((double)end_OpenCompactionOutputFile2.tv_nsec)/s_to_ns ) - ( (int)begin_OpenCompactionOutputFile2.tv_sec+((double)begin_OpenCompactionOutputFile2.tv_nsec)/s_to_ns );
	
  if (s.ok()) {
    compact->builder = new TableBuilder(options_, compact->outfile);
  }
  
  
  return s;
}

Status DBImpl::FinishCompactionOutputFile(CompactionState* compact,
                                          Iterator* input) {
  assert(compact != NULL);
  assert(compact->outfile != NULL);
  assert(compact->builder != NULL);
 //struct timespec begin_S1, end_S1;
//clock_gettime(CLOCK_MONOTONIC,&begin_S1);
	  
  
  const uint64_t output_number = compact->current_output()->number;
  assert(output_number != 0);

  // Check for iterator errors
  Status s = input->status();
  const uint64_t current_entries = compact->builder->NumEntries();
  if (s.ok()) {
    s = compact->builder->Finish();
  } else {
    compact->builder->Abandon();
  }
  const uint64_t current_bytes = compact->builder->FileSize();
  compact->current_output()->file_size = current_bytes;
  compact->total_bytes += current_bytes;
  delete compact->builder;
  compact->builder = NULL;

//clock_gettime(CLOCK_MONOTONIC,&end_S1);
//durations[30] +=( (int)end_S1.tv_sec +((double)end_S1.tv_nsec)/s_to_ns ) - ( (int)begin_S1.tv_sec+((double)begin_S1.tv_nsec)/s_to_ns );
	
  // Finish and check for file errors
 //struct timespec begin_sync, end_sync;
//clock_gettime(CLOCK_MONOTONIC,&begin_sync);
	  
  
  if (s.ok()) {
    s = compact->outfile->Sync();
  }
 
//clock_gettime(CLOCK_MONOTONIC,&end_sync);
//durations[32] +=( (int)end_sync.tv_sec +((double)end_sync.tv_nsec)/s_to_ns ) - ( (int)begin_sync.tv_sec+((double)begin_sync.tv_nsec)/s_to_ns );
 //struct timespec begin_close, end_close;
//clock_gettime(CLOCK_MONOTONIC,&begin_close);
	  
	 
  if (s.ok()) {
    s = compact->outfile->Close();
  }

//clock_gettime(CLOCK_MONOTONIC,&end_close);
//durations[33] +=( (int)end_close.tv_sec +((double)end_close.tv_nsec)/s_to_ns ) - ( (int)begin_close.tv_sec+((double)begin_close.tv_nsec)/s_to_ns );
	    
  
 //struct timespec begin_S2, end_S2;
//clock_gettime(CLOCK_MONOTONIC,&begin_S2);
	  
    
  delete compact->outfile;
  compact->outfile = NULL;
//clock_gettime(CLOCK_MONOTONIC,&end_S2);
//durations[31] +=( (int)end_S2.tv_sec +((double)end_S2.tv_nsec)/s_to_ns ) - ( (int)begin_S2.tv_sec+((double)begin_S2.tv_nsec)/s_to_ns );
	
  if (s.ok() && current_entries > 0) {
    // Verify that the table is usable
	//printf("in db_impl.cc, before NewIterator, current_bytes=%d\n",current_bytes);
    Iterator* iter = table_cache_->NewIterator(ReadOptions(),
                                               output_number,
                                               current_bytes);
    s = iter->status();
    delete iter;
    if (s.ok()) {
      Log(options_.info_log,
          "Generated table #%llu: %lld keys, %lld bytes",
          (unsigned long long) output_number,
          (unsigned long long) current_entries,
          (unsigned long long) current_bytes);
    }
  }
 

  return s;
}


Status DBImpl::InstallCompactionResults(CompactionState* compact) {
  mutex_.AssertHeld();
  Log(options_.info_log,  "Compacted %d@%d + %d@%d files => %lld bytes",
      compact->compaction->num_input_files(0),
      compact->compaction->level(),
      compact->compaction->num_input_files(1),
      compact->compaction->level() + 1,
      static_cast<long long>(compact->total_bytes));

  // Add compaction outputs
  compact->compaction->AddInputDeletions(compact->compaction->edit());
  const int level = compact->compaction->level();
  for (size_t i = 0; i < compact->outputs.size(); i++) {
    const CompactionState::Output& out = compact->outputs[i];
    compact->compaction->edit()->AddFile(
        level + 1,
        out.number, out.file_size, out.smallest, out.largest);
  }
  return versions_->LogAndApply(compact->compaction->edit(), &mutex_);
}

Status DBImpl::DoCompactionWork(CompactionState* compact) {
  const uint64_t start_micros = env_->NowMicros();
  int64_t imm_micros = 0;  // Micros spent doing imm_ compactions
//printf("dbimpl, DoCompactionWork begin\n ");
  Log(options_.info_log,  "Compacting %d@%d + %d@%d files",
      compact->compaction->num_input_files(0),
      compact->compaction->level(),
      compact->compaction->num_input_files(1),
      compact->compaction->level() + 1);

  assert(versions_->NumLevelFiles(compact->compaction->level()) > 0);
  assert(compact->builder == NULL);
  assert(compact->outfile == NULL);
  if (snapshots_.empty()) {
    compact->smallest_snapshot = versions_->LastSequence();
  } else {
    compact->smallest_snapshot = snapshots_.oldest()->number_;
  }

  // Release mutex while we're actually doing the compaction work
  mutex_.Unlock();

  Iterator* input = versions_->MakeInputIterator(compact->compaction);
  input->SeekToFirst();
  Status status;
  ParsedInternalKey ikey;
  std::string current_user_key;
  bool has_current_user_key = false;
  SequenceNumber last_sequence_for_key = kMaxSequenceNumber;
  //printf("dbimpl, DoCompactionWork before for\n ");
  for (; input->Valid() && !shutting_down_.Acquire_Load(); ) {
    // Prioritize immutable compaction work
    if (has_imm_.NoBarrier_Load() != NULL) {
      const uint64_t imm_start = env_->NowMicros();
      mutex_.Lock();
      if (imm_ != NULL) {
        CompactMemTable();
        bg_cv_.SignalAll();  // Wakeup MakeRoomForWrite() if necessary
      }
      mutex_.Unlock();
      imm_micros += (env_->NowMicros() - imm_start);
    }
//printf("dbimpl, DoCompactionWork 1111111111\n ");

    Slice key = input->key();
    if (compact->compaction->ShouldStopBefore(key) &&
        compact->builder != NULL) {
      status = FinishCompactionOutputFile(compact, input);
	  //printf("00000,\n");
	  //durations[40]++;
      if (!status.ok()) {
        break;
      }
    }

    // Handle key/value, add to state, etc.
    bool drop = false;
    if (!ParseInternalKey(key, &ikey)) {
      // Do not hide error keys
      current_user_key.clear();
      has_current_user_key = false;
      last_sequence_for_key = kMaxSequenceNumber;
    } else {
      if (!has_current_user_key ||
          user_comparator()->Compare(ikey.user_key,
                                     Slice(current_user_key)) != 0) {
        // First occurrence of this user key
        current_user_key.assign(ikey.user_key.data(), ikey.user_key.size());
        has_current_user_key = true;
        last_sequence_for_key = kMaxSequenceNumber;
      }

      if (last_sequence_for_key <= compact->smallest_snapshot) {
        // Hidden by an newer entry for same user key
        drop = true;    // (A)
      } else if (ikey.type == kTypeDeletion &&
                 ikey.sequence <= compact->smallest_snapshot &&
                 compact->compaction->IsBaseLevelForKey(ikey.user_key)) {
        // For this user key:
        // (1) there is no data in higher levels
        // (2) data in lower levels will have larger sequence numbers
        // (3) data in layers that are being compacted here and have
        //     smaller sequence numbers will be dropped in the next
        //     few iterations of this loop (by rule (A) above).
        // Therefore this deletion marker is obsolete and can be dropped.
        drop = true;
      }

      last_sequence_for_key = ikey.sequence;
    }
#if 0
    Log(options_.info_log,
        "  Compact: %s, seq %d, type: %d %d, drop: %d, is_base: %d, "
        "%d smallest_snapshot: %d",
        ikey.user_key.ToString().c_str(),
        (int)ikey.sequence, ikey.type, kTypeValue, drop,
        compact->compaction->IsBaseLevelForKey(ikey.user_key),
        (int)last_sequence_for_key, (int)compact->smallest_snapshot);
#endif
//printf("dbimpl, DoCompactionWork 55555555555\n ");

    if (!drop) {
////struct timespec begin_Ndrop, end_Ndrop;
////clock_gettime(CLOCK_MONOTONIC,&begin_Ndrop);	
      // Open output file if necessary
// //struct timespec begin_NdropT0, end_NdropT0;
////clock_gettime(CLOCK_MONOTONIC,&begin_NdropT0);
	  

      if (compact->builder == NULL) {
        status = OpenCompactionOutputFile(compact);
        if (!status.ok()) {
          break;
        }
      }

////clock_gettime(CLOCK_MONOTONIC,&end_NdropT0);
////durations[24] +=( (int)end_NdropT0.tv_sec +((double)end_NdropT0.tv_nsec)/s_to_ns ) - ( (int)begin_NdropT0.tv_sec+((double)begin_NdropT0.tv_nsec)/s_to_ns );
	  	  
	 
// //struct timespec begin_NdropT1, end_NdropT1;
////clock_gettime(CLOCK_MONOTONIC,&begin_NdropT1);
	  
      if (compact->builder->NumEntries() == 0) {
        compact->current_output()->smallest.DecodeFrom(key);
      }
	  
      compact->current_output()->largest.DecodeFrom(key);

////clock_gettime(CLOCK_MONOTONIC,&end_NdropT1);
////durations[22] +=( (int)end_NdropT1.tv_sec +((double)end_NdropT1.tv_nsec)/s_to_ns ) - ( (int)begin_NdropT1.tv_sec+((double)begin_NdropT1.tv_nsec)/s_to_ns );
	  
//printf("dbimpl, DoCompactionWork 777777777\n ");
	  
//struct timespec begin_NdropAdd, end_NdropAdd;
//clock_gettime(CLOCK_MONOTONIC,&begin_NdropAdd);

  
      compact->builder->Add(key, input->value());
//clock_gettime(CLOCK_MONOTONIC,&end_NdropAdd);
//durations[21] +=( (int)end_NdropAdd.tv_sec +((double)end_NdropAdd.tv_nsec)/s_to_ns ) - ( (int)begin_NdropAdd.tv_sec+((double)begin_NdropAdd.tv_nsec)/s_to_ns );

//struct timespec begin_NdropT2, end_NdropT2;
//clock_gettime(CLOCK_MONOTONIC,&begin_NdropT2);
//printf("dbimpl, DoCompactionWork 999999999\n ");

      // Close output file if it is big enough
      if (compact->builder->FileSize() >=
          compact->compaction->MaxOutputFileSize()) {
//struct timespec begin_c1, end_c1;
//clock_gettime(CLOCK_MONOTONIC,&begin_c1);
        status = FinishCompactionOutputFile(compact, input);
		
//clock_gettime(CLOCK_MONOTONIC,&end_c1);
//durations[43] =( (int)end_c1.tv_sec +((double)end_c1.tv_nsec)/s_to_ns ) - ( (int)begin_c1.tv_sec+((double)begin_c1.tv_nsec)/s_to_ns );

		//printf("11111, durations[43]=%f\n",durations[43]);
		//printf("dbimpl,2222 ,status.ok()=%d\n",status.ok());
		//durations[41]++;
        if (!status.ok()) {
			printf("dbimpl,status is not ok ,status.ok()=%d\n",status.ok());
          break;
        }
      }

//clock_gettime(CLOCK_MONOTONIC,&end_NdropT2);
//durations[23] +=( (int)end_NdropT2.tv_sec +((double)end_NdropT2.tv_nsec)/s_to_ns ) - ( (int)begin_NdropT2.tv_sec+((double)begin_NdropT2.tv_nsec)/s_to_ns );
	

//clock_gettime(CLOCK_MONOTONIC,&end_Ndrop);
//durations[20] +=( (int)end_Ndrop.tv_sec +((double)end_Ndrop.tv_nsec)/s_to_ns ) - ( (int)begin_Ndrop.tv_sec+((double)begin_Ndrop.tv_nsec)/s_to_ns );

    }
//printf("dbimpl, DoCompactionWork zzzzzzzzzzz\n ");

    input->Next();
	//printf("dbimpl, DoCompactionWork zzzzzzz\n ");
//exit(9);
  }
  //printf("dbimpl, DoCompactionWork after for\n ");
//printf("dbimpl,3333 ,status.ok()=%d\n",status.ok());

  if (status.ok() && shutting_down_.Acquire_Load()) {
    status = Status::IOError("Deleting DB during compaction");
  }
  if (status.ok() && compact->builder != NULL) {
    status = FinishCompactionOutputFile(compact, input);
//printf("dbimpl,5555 ,status.ok()=%d\n",status.ok());
	//durations[42]++;
	  //if( status.ok()==0) printf("_____I am db_impl.cc, DoCompactionWork a FinishCompactionOutputFile   status.ok()=%d\n", status.ok());
  }
  if (status.ok()) {
    //status = input->status();
	//if( status.ok()==0) printf("_____I am db_impl.cc, DoCompactionWork a input->status   status.ok()=%d\n", status.ok());
  }
  delete input;
  input = NULL;

  CompactionStats stats;
  stats.micros = env_->NowMicros() - start_micros - imm_micros;
  for (int which = 0; which < 2; which++) {
    for (int i = 0; i < compact->compaction->num_input_files(which); i++) {
      stats.bytes_read += compact->compaction->input(which, i)->file_size;
    }
  }
  for (size_t i = 0; i < compact->outputs.size(); i++) {
    stats.bytes_written += compact->outputs[i].file_size;
  }

  mutex_.Lock();
  stats_[compact->compaction->level() + 1].Add(stats);

  if (status.ok()) {
	  //printf("_____I am db_impl.cc,DoCompactionWork, b InstallCompactionResults, end   status.ok()=%d\n", status.ok());
    status = InstallCompactionResults(compact);
	  //printf("_____I am db_impl.cc,DoCompactionWork, a InstallCompactionResults, end   status.ok()=%d\n", status.ok());
  }
  if (!status.ok()) {
	  if( status.ok()==0) printf("_____I am db_impl.cc, DoCompactionWork, RecordBackgroundError  status.ok()=%d\n", status.ok());
    RecordBackgroundError(status);
  }
  VersionSet::LevelSummaryStorage tmp;
  Log(options_.info_log,
      "compacted to: %s", versions_->LevelSummary(&tmp));
	//printf("dbimpl, DoCompactionWork end\n ");  
	   //if( status.ok()==0) printf("_____I am db_impl.cc, DoCompactionWork, end   status.ok()=%d\n", status.ok());
  return status;
}//end DoCompactionWork



namespace {
struct IterState {
  port::Mutex* mu;
  Version* version;
  MemTable* mem;
  MemTable* imm;
};

static void CleanupIteratorState(void* arg1, void* arg2) {
  IterState* state = reinterpret_cast<IterState*>(arg1);
  state->mu->Lock();
  state->mem->Unref();
  if (state->imm != NULL) state->imm->Unref();
  state->version->Unref();
  state->mu->Unlock();
  delete state;
}
}  // namespace

Iterator* DBImpl::NewInternalIterator(const ReadOptions& options,
                                      SequenceNumber* latest_snapshot,
                                      uint32_t* seed) {
  IterState* cleanup = new IterState;
  mutex_.Lock();
  *latest_snapshot = versions_->LastSequence();

  // Collect together all needed child iterators
  std::vector<Iterator*> list;
  list.push_back(mem_->NewIterator());
  mem_->Ref();
  if (imm_ != NULL) {
    list.push_back(imm_->NewIterator());
    imm_->Ref();
  }
  versions_->current()->AddIterators(options, &list);
  Iterator* internal_iter =
      NewMergingIterator(&internal_comparator_, &list[0], list.size());
  versions_->current()->Ref();

  cleanup->mu = &mutex_;
  cleanup->mem = mem_;
  cleanup->imm = imm_;
  cleanup->version = versions_->current();
  internal_iter->RegisterCleanup(CleanupIteratorState, cleanup, NULL);

  *seed = ++seed_;
  mutex_.Unlock();
  return internal_iter;
}

Iterator* DBImpl::TEST_NewInternalIterator() {
  SequenceNumber ignored;
  uint32_t ignored_seed;
  return NewInternalIterator(ReadOptions(), &ignored, &ignored_seed);
}

int64_t DBImpl::TEST_MaxNextLevelOverlappingBytes() {
  MutexLock l(&mutex_);
  return versions_->MaxNextLevelOverlappingBytes();
}

Status DBImpl::Get(const ReadOptions& options,
                   const Slice& key,
                   std::string* value) {
	//printf("I am db_impl.cc, DBImpl::Get, begin\n");
	//exit(1);


	
  Status s;
  MutexLock l(&mutex_);
  SequenceNumber snapshot;
  if (options.snapshot != NULL) {
    snapshot = reinterpret_cast<const SnapshotImpl*>(options.snapshot)->number_;
  } else {
    snapshot = versions_->LastSequence();
  }

  MemTable* mem = mem_;
  MemTable* imm = imm_;
  Version* current = versions_->current();
  mem->Ref();
  if (imm != NULL) imm->Ref();
  current->Ref();

  bool have_stat_update = false;
  Version::GetStats stats;

  
  // Unlock while reading from files and memtables
  {
    mutex_.Unlock();
    // First look in the memtable, then in the immutable memtable (if any).
    LookupKey lkey(key, snapshot);

    if (mem->Get(lkey, value, &s)) {
		//printf("I am db_impl.cc, Get,  come to mem\n");
		
      // Done
    } else if (imm != NULL && imm->Get(lkey, value, &s)) {
      // Done
    } else {
		
      s = current->Get(options, lkey, value, &stats);
 
	  //printf("I am db_impl.cc, Get,  come to device, s.ok()=%d, key=%s\n",s.ok(), key.data());
	  if(s.ok()==1){
		//printf("I am dn_impl.cc, Get,  get it in ldb\n");
	  }
      have_stat_update = true;
    }

    mutex_.Lock();
  }

  if (have_stat_update && current->UpdateStats(stats)) {
    //MaybeScheduleCompaction();
  }
  mem->Unref();
  if (imm != NULL) imm->Unref();
  current->Unref();

  return s;
}

Iterator* DBImpl::NewIterator(const ReadOptions& options) {
  SequenceNumber latest_snapshot;
  uint32_t seed;
  Iterator* iter = NewInternalIterator(options, &latest_snapshot, &seed);
  return NewDBIterator(
      this, user_comparator(), iter,
      (options.snapshot != NULL
       ? reinterpret_cast<const SnapshotImpl*>(options.snapshot)->number_
       : latest_snapshot),
      seed);
}

void DBImpl::RecordReadSample(Slice key) {
  MutexLock l(&mutex_);
  if (versions_->current()->RecordReadSample(key)) {
	printf("db_impc.cc, RecordReadSample\n");
    //MaybeScheduleCompaction();
  }
}

const Snapshot* DBImpl::GetSnapshot() {
  MutexLock l(&mutex_);
  return snapshots_.New(versions_->LastSequence());
}

void DBImpl::ReleaseSnapshot(const Snapshot* s) {
  MutexLock l(&mutex_);
  snapshots_.Delete(reinterpret_cast<const SnapshotImpl*>(s));
}

// Convenience methods
Status DBImpl::Put(const WriteOptions& o, const Slice& key, const Slice& val) {
  return DB::Put(o, key, val);
}

Status DBImpl::Delete(const WriteOptions& options, const Slice& key) {
  return DB::Delete(options, key);
}

Status DBImpl::Write(const WriteOptions& options, WriteBatch* my_batch) {

//timing 
struct timespec begin, end; 
clock_gettime(CLOCK_MONOTONIC,&begin);
//timing

  Writer w(&mutex_);
  w.batch = my_batch;
  w.sync = options.sync;
  w.done = false;
 //printf("I am db_impl.cc, Write, begin\n");
 //exit(1);//test if enter in this --yes
 // printf("I am db_impl.cc, Write, after test1\n");
  MutexLock l(&mutex_);
  writers_.push_back(&w);
  while (!w.done && &w != writers_.front()) {
    w.cv.Wait();
  }
  if (w.done) {
	  //printf("I am db_impl.cc, Write, w.status.ok()=%d\n", w.status.ok());
    return w.status;
  }

  // May temporarily unlock and wait.
  //timing+

  //timing-
  struct timespec begin_makeroom, end_makeroom; 
clock_gettime(CLOCK_MONOTONIC,&begin_makeroom);
  Status status;
  status = MakeRoomForWrite(my_batch == NULL);
  clock_gettime(CLOCK_MONOTONIC,&end_makeroom);
durations[2] +=( (int)end_makeroom.tv_sec +((double)end_makeroom.tv_nsec)/s_to_ns ) - ( (int)begin_makeroom.tv_sec+((double)begin_makeroom.tv_nsec)/s_to_ns );
  //timing+

//timing- 
  uint64_t last_sequence = versions_->LastSequence();
  Writer* last_writer = &w;
  //if( status.ok()==0) printf("I am db_impl.cc, b if ,  status.ok()=%d\n", status.ok());
  if (status.ok() && my_batch != NULL) {  // NULL batch is for compactions
    WriteBatch* updates = BuildBatchGroup(&last_writer);
    WriteBatchInternal::SetSequence(updates, last_sequence + 1);
    last_sequence += WriteBatchInternal::Count(updates);

    // Add to log and apply to memtable.  We can release the lock
    // during this phase since &w is currently responsible for logging
    // and protects against concurrent loggers and concurrent writes
    // into mem_.
    {
      mutex_.Unlock();
	  //exit(1);//befor log write
	 	  //timing+
 struct timespec begin_log_, end_log_; 
clock_gettime(CLOCK_MONOTONIC,&begin_log_);
//timing-
      status = log_->AddRecord(WriteBatchInternal::Contents(updates));//in Open, log_=new log::Writer(lfile); lfile represents the log file
	 //timing+   
clock_gettime(CLOCK_MONOTONIC,&end_log_);
durations[3] +=( (int)end_log_.tv_sec +((double)end_log_.tv_nsec)/s_to_ns ) - ( (int)begin_log_.tv_sec+((double)begin_log_.tv_nsec)/s_to_ns );
 //timing-
	   //exit(1);//after log write
      bool sync_error = false;
	  //timing+
 //struct timespec begin_Sync, end_Sync; 
//clock_gettime(CLOCK_MONOTONIC,&begin_Sync);
//timing
	//if(options.sync) printf("options.sync=%d\n",options.sync);
      if (status.ok() && options.sync) {
        status = logfile_->Sync();
        if (!status.ok()) {
          sync_error = true;
        }
      }
 //timing+  
//clock_gettime(CLOCK_MONOTONIC,&end_Sync);
//durations[5] +=( (int)end_Sync.tv_sec +((double)end_Sync.tv_nsec)/s_to_ns ) - ( (int)begin_Sync.tv_sec+((double)begin_Sync.tv_nsec)/s_to_ns );
 //timing-	  
		  //timing+
 struct timespec begin_mem_, end_mem_; 
clock_gettime(CLOCK_MONOTONIC,&begin_mem_);
//timing
      if (status.ok()) {
        status = WriteBatchInternal::InsertInto(updates, mem_);
		//if( status.ok()==0) printf("I am db_impl.cc, a WriteBatchInternal,  status.ok()=%d\n", status.ok());
      }
//timing+	  
clock_gettime(CLOCK_MONOTONIC,&end_mem_);
durations[4] +=( (int)end_mem_.tv_sec +((double)end_mem_.tv_nsec)/s_to_ns ) - ( (int)begin_mem_.tv_sec+((double)begin_mem_.tv_nsec)/s_to_ns );
//timing-	  
      mutex_.Lock();
      if (sync_error) {
        // The state of the log file is indeterminate: the log record we
        // just added may or may not show up when the DB is re-opened.
        // So we force the DB into a mode where all future writes fail.
        RecordBackgroundError(status);
      }
    }
    if (updates == tmp_batch_) tmp_batch_->Clear();

    versions_->SetLastSequence(last_sequence);
  }
 //if( status.ok()==0) printf("I am db_impl.cc, b while,  status.ok()=%d\n", status.ok());
  while (true) {
    Writer* ready = writers_.front();
    writers_.pop_front();
    if (ready != &w) {
      ready->status = status;
      ready->done = true;
      ready->cv.Signal();
    }
    if (ready == last_writer) break;
  }

  // Notify new head of write queue
  if (!writers_.empty()) {
    writers_.front()->cv.Signal();
  }
	//if( status.ok()==0) printf("I am db_impl.cc, Write, end,  status.ok()=%d\n", status.ok());
	  //timing+
clock_gettime(CLOCK_MONOTONIC,&end); //begin insert , so begin counting
durations[1] +=( (int)end.tv_sec +((double)end.tv_nsec)/s_to_ns ) - ( (int)begin.tv_sec+((double)begin.tv_nsec)/s_to_ns );
//timing-
  return status;
}

// REQUIRES: Writer list must be non-empty
// REQUIRES: First writer must have a non-NULL batch
WriteBatch* DBImpl::BuildBatchGroup(Writer** last_writer) {
  assert(!writers_.empty());
  Writer* first = writers_.front();
  WriteBatch* result = first->batch;
  assert(result != NULL);

  size_t size = WriteBatchInternal::ByteSize(first->batch);

  // Allow the group to grow up to a maximum size, but if the
  // original write is small, limit the growth so we do not slow
  // down the small write too much.
  size_t max_size = 1 << 20;
  if (size <= (128<<10)) {
    max_size = size + (128<<10);
  }

  *last_writer = first;
  std::deque<Writer*>::iterator iter = writers_.begin();
  ++iter;  // Advance past "first"
  for (; iter != writers_.end(); ++iter) {
    Writer* w = *iter;
    if (w->sync && !first->sync) {
      // Do not include a sync write into a batch handled by a non-sync write.
      break;
    }

    if (w->batch != NULL) {
      size += WriteBatchInternal::ByteSize(w->batch);
      if (size > max_size) {
        // Do not make batch too big
        break;
      }

      // Append to *result
      if (result == first->batch) {
        // Switch to temporary batch instead of disturbing caller's batch
        result = tmp_batch_;
        assert(WriteBatchInternal::Count(result) == 0);
        WriteBatchInternal::Append(result, first->batch);
      }
      WriteBatchInternal::Append(result, w->batch);
    }
    *last_writer = w;
  }
  return result;
}

// REQUIRES: mutex_ is held
// REQUIRES: this thread is currently at the front of the writer queue
Status DBImpl::MakeRoomForWrite(bool force) {
	//printf(" I am db_impl.cc MakeRoomForWrite \n");
	



  mutex_.AssertHeld();
  assert(!writers_.empty());
  bool allow_delay = !force;
  Status s;
  int static enter=0;
 // printf(" I am db_impl.cc MakeRoomForWrite,  versions_->NumLevelFiles(0)=%d \n", versions_->NumLevelFiles(0));


  while (true) {
//struct timespec begin_while, end_while;
//clock_gettime(CLOCK_MONOTONIC,&begin_while);

    if (!bg_error_.ok()) {
      // Yield previous error
 
      s = bg_error_;
	  if( s.ok()==0) printf("_____I am db_impl.cc, MakeRoomForWrite, in bg_error_  s.ok()=%d\n", s.ok());
	   //printf(" XXXXXX break, Yield previous error ,I am db_impl.cc MakeRoomForWrite, enter= %d\n",enter);
  
      break;
    } else if (
        allow_delay &&
        versions_->NumLevelFiles(0) >= config::kL0_SlowdownWritesTrigger) { //
			
      // We are getting close to hitting a hard limit on the number of
      // L0 files.  Rather than delaying a single write by several
      // seconds when we hit the hard limit, start delaying each
      // individual write by 1ms to reduce latency variance.  Also,
      // this delay hands over some CPU to the compaction thread in
      // case it is sharing the same core as the writer.
	  
	  //printf("NumLevelFiles(0)=%d\n",versions_->NumLevelFiles(0));
	  //struct timespec begin_sleep, end_sleep;
//clock_gettime(CLOCK_MONOTONIC,&begin_sleep);

      mutex_.Unlock();
      env_->SleepForMicroseconds(1000);
      allow_delay = false;  // Do not delay a single write more than once
      mutex_.Lock();
	       
//clock_gettime(CLOCK_MONOTONIC,&end_sleep);
//durations[9] +=( (int)end_sleep.tv_sec +((double)end_sleep.tv_nsec)/s_to_ns ) - ( (int)begin_sleep.tv_sec+((double)begin_sleep.tv_nsec)/s_to_ns );

    } else if (!force &&
               (mem_->ApproximateMemoryUsage() <= options_.write_buffer_size)) {
			   
			   	  //struct timespec begin_blank, end_blank;
//clock_gettime(CLOCK_MONOTONIC,&begin_blank);

   
	  //clock_gettime(CLOCK_MONOTONIC,&end_blank);
//durations[16] +=( (int)end_blank.tv_sec +((double)end_blank.tv_nsec)/s_to_ns ) - ( (int)begin_blank.tv_sec+((double)begin_blank.tv_nsec)/s_to_ns );

      // There is room in current memtable
	 // printf(" XXXXXX break, There is room in current memtable ,I am db_impl.cc MakeRoomForWrite,enter= %d\n",enter);
      break;
    } else if (imm_ != NULL) {
      // We have filled up the current memtable, but the previous
      // one is still being compacted, so we wait.
	  	  	  //struct timespec begin_wait1, end_wait1;
//clock_gettime(CLOCK_MONOTONIC,&begin_wait1);

      Log(options_.info_log, "Current memtable full; waiting...\n");
      bg_cv_.Wait();
	  
	  //clock_gettime(CLOCK_MONOTONIC,&end_wait1);
//durations[13] +=( (int)end_wait1.tv_sec +((double)end_wait1.tv_nsec)/s_to_ns ) - ( (int)begin_wait1.tv_sec+((double)begin_wait1.tv_nsec)/s_to_ns );

    } else if (versions_->NumLevelFiles(0) >=config::kL0_StopWritesTrigger) {// 
      // There are too many level-0 files.
	  	  //struct timespec begin_wait2, end_wait2;
//clock_gettime(CLOCK_MONOTONIC,&begin_wait2);

      Log(options_.info_log, "Too many L0 files; waiting...\n");
      bg_cv_.Wait();
	  
	  //clock_gettime(CLOCK_MONOTONIC,&end_wait2);
//durations[12] +=( (int)end_wait2.tv_sec +((double)end_wait2.tv_nsec)/s_to_ns ) - ( (int)begin_wait2.tv_sec+((double)begin_wait2.tv_nsec)/s_to_ns );

    } else {
      // Attempt to switch to a new memtable and trigger compaction of old
	  //printf(" I am db_impl.cc MakeRoomForWrite,  switch to a new memtable and trigger compaction of old \n");
	    //struct timespec begin_MaybeScheduleCompaction, end_MaybeScheduleCompaction;
//clock_gettime(CLOCK_MONOTONIC,&begin_MaybeScheduleCompaction);

	  ////struct timespec begin_NewWritableFile, end_NewWritableFile;
////clock_gettime(CLOCK_MONOTONIC,&begin_NewWritableFile);

	  enter++;
      assert(versions_->PrevLogNumber() == 0);
      uint64_t new_log_number; 
	  //printf("db_impl, before apply log number\n");
	    //new_log_number  = versions_->NewFileNumber();
		//exit(1);
		new_log_number=2;
      WritableFile* lfile = NULL;
      s = env_->NewWritableFile(LogFileName(dbname_, new_log_number), &lfile);
	   //printf("db_impl, after NewWritableFile, s.ok()=%d\n",s.ok());
////clock_gettime(CLOCK_MONOTONIC,&end_NewWritableFile);
////durations[11] +=( (int)end_NewWritableFile.tv_sec +((double)end_NewWritableFile.tv_nsec)/s_to_ns ) - ( (int)begin_NewWritableFile.tv_sec+((double)begin_NewWritableFile.tv_nsec)/s_to_ns );

	  if( s.ok()==0) printf("_____I am db_impl.cc, MakeRoomForWrite, a NewWritableFile  s.ok()=%d\n", s.ok());
	  
      if (!s.ok()) {
        // Avoid chewing through file number space in a tight loop.
        versions_->ReuseFileNumber(new_log_number);
	  
		 //printf(" XXXXXX break, Avoid chewing through file number space in a tight loop. ,I am db_impl.cc MakeRoomForWrite,enter= %d\n",enter);
        break;
      }
////struct timespec begin_bm, end_bm;
////clock_gettime(CLOCK_MONOTONIC,&begin_bm);
      delete log_;
      delete logfile_;
      logfile_ = lfile;
      logfile_number_ = new_log_number;
	  //printf("db_impl.cc MakeRoomForWrite, logfile_number_=%d\n",logfile_number_);
      log_ = new log::Writer(lfile);
      imm_ = mem_;
      has_imm_.Release_Store(imm_);
      mem_ = new MemTable(internal_comparator_);
      mem_->Ref();
      force = false;   // Do not force another compaction if have room
	  //printf(" I am db_impl.cc MakeRoomForWrite, b  MaybeScheduleCompaction,enter= %d\n",enter);
      //exit(9);
////clock_gettime(CLOCK_MONOTONIC,&end_bm);
////durations[8] +=( (int)end_bm.tv_sec +((double)end_bm.tv_nsec)/s_to_ns ) - ( (int)begin_bm.tv_sec+((double)begin_bm.tv_nsec)/s_to_ns );

	  
	
		MaybeScheduleCompaction();
//clock_gettime(CLOCK_MONOTONIC,&end_MaybeScheduleCompaction);
//durations[7] +=( (int)end_MaybeScheduleCompaction.tv_sec +((double)end_MaybeScheduleCompaction.tv_nsec)/s_to_ns ) - ( (int)begin_MaybeScheduleCompaction.tv_sec+((double)begin_MaybeScheduleCompaction.tv_nsec)/s_to_ns );

    }
	
	 //clock_gettime(CLOCK_MONOTONIC,&end_while);
//durations[14] +=( (int)end_while.tv_sec +((double)end_while.tv_nsec)/s_to_ns ) - ( (int)begin_while.tv_sec+((double)begin_while.tv_nsec)/s_to_ns );

  }
  
  
  //printf("enter= %d\n",enter);
  //if( s.ok()==0) printf("_____I am db_impl.cc, MakeRoomForWrite, end  s.ok()=%d\n", s.ok());
  
  return s;
}



bool DBImpl::GetProperty(const Slice& property, std::string* value) {
  value->clear();

  MutexLock l(&mutex_);
  Slice in = property;
  Slice prefix("leveldb.");
  if (!in.starts_with(prefix)) return false;
  in.remove_prefix(prefix.size());

  if (in.starts_with("num-files-at-level")) {
    in.remove_prefix(strlen("num-files-at-level"));
    uint64_t level;
    bool ok = ConsumeDecimalNumber(&in, &level) && in.empty();
    if (!ok || level >= config::kNumLevels) {
      return false;
    } else {
      char buf[100];
      snprintf(buf, sizeof(buf), "%d",
               versions_->NumLevelFiles(static_cast<int>(level)));
      *value = buf;
      return true;
    }
  } else if (in == "stats") {
    char buf[200];
    snprintf(buf, sizeof(buf),
             "                               Compactions\n"
             "Level  Files Size(MB) Time(sec) Read(MB) Write(MB)\n"
             "--------------------------------------------------\n"
             );
    value->append(buf);
    for (int level = 0; level < config::kNumLevels; level++) {
      int files = versions_->NumLevelFiles(level);
      if (stats_[level].micros > 0 || files > 0) {
        snprintf(
            buf, sizeof(buf),
            "%3d %8d %8.0f %9.0f %8.0f %9.0f\n",
            level,
            files,
            versions_->NumLevelBytes(level) / 1048576.0,
            stats_[level].micros / 1e6,
            stats_[level].bytes_read / 1048576.0,
            stats_[level].bytes_written / 1048576.0);
        value->append(buf);
      }
    }
    return true;
  } else if (in == "sstables") {
    *value = versions_->current()->DebugString();
    return true;
  }

  return false;
}

void DBImpl::GetApproximateSizes(
    const Range* range, int n,
    uint64_t* sizes) {
  // TODO(opt): better implementation
  Version* v;
  {
    MutexLock l(&mutex_);
    versions_->current()->Ref();
    v = versions_->current();
  }

  for (int i = 0; i < n; i++) {
    // Convert user_key into a corresponding internal key.
    InternalKey k1(range[i].start, kMaxSequenceNumber, kValueTypeForSeek);
    InternalKey k2(range[i].limit, kMaxSequenceNumber, kValueTypeForSeek);
    uint64_t start = versions_->ApproximateOffsetOf(v, k1);
    uint64_t limit = versions_->ApproximateOffsetOf(v, k2);
    sizes[i] = (limit >= start ? limit - start : 0);
  }

  {
    MutexLock l(&mutex_);
    v->Unref();
  }
}

// Default implementations of convenience methods that subclasses of DB
// can call if they wish
Status DB::Put(const WriteOptions& opt, const Slice& key, const Slice& value) {
	printf("__Hello, I am DB::Put, begin\n");
  WriteBatch batch;
  batch.Put(key, value);
  
  printf("__Hello, I am DB::Put, end\n");
  return Write(opt, &batch);
	
}

Status DB::Delete(const WriteOptions& opt, const Slice& key) {
  WriteBatch batch;
  batch.Delete(key);
  return Write(opt, &batch);
}

DB::~DB() { }

Status DB::Open(const Options& options, const std::string& dbname,
                DB** dbptr) {
  *dbptr = NULL;
   printf(">>>>>>>>>>>>>>Hello, I am DB::Open ,begin, dbname=%s\n",dbname.c_str());
   //exit(1);//test if how much files have been created --none
   
   
printf("__Hello, I am DB::Open, options.create_if_missing=%d\n",options.create_if_missing);
  DBImpl* impl = new DBImpl(options, dbname);
	
printf("__Hello, I am DB::Open, after new DBImpl\n");
  impl->mutex_.Lock();
  VersionEdit edit;
  printf("_____Hello, I am DB::Open ,before Recover\n");
    //printf("I am db_impl.cc, DB::Open, before recover, L0=%d,L1=%d,L2=%d,L3=%d \n",  impl->versions_->current_->files_[0].size(), impl->versions_->current_->files_[1].size(), impl->versions_->current_->files_[2].size(), impl->versions_->current_->files_[3].size()	); 

  Status s;
	s= impl->Recover(&edit); // Handles create_if_missing, error_if_exists
	// if(flash_using_exist==1){
		// printf("dbimpl.cc, recover begin..\n");
		// s= impl->Recover(&edit); // Handles create_if_missing, error_if_exists
	// }
    //printf("I am db_impl.cc, DB::Open, after recover, L0=%d,L1=%d,L2=%d,L3=%d \n",  impl->versions_->current_->files_[0].size(), impl->versions_->current_->files_[1].size(), impl->versions_->current_->files_[2].size(), impl->versions_->current_->files_[3].size()	); 
 //exit(1);
   printf("_____Hello, I am DB::Open ,after Recover\n");
   
  printf("_____Hello, I am DB::Open 100001111,s.ok()=%d\n",s.ok());
  if (s.ok()) {//
    uint64_t new_log_number = 2;//impl->versions_->NewFileNumber();
    WritableFile* lfile;
	printf("_____Hello, I am DB::Open aaaaaaa,s.ok()=%d\n",s.ok());
    s = options.env->NewWritableFile(LogFileName(dbname, new_log_number),
                                     &lfile);//NewWritableFile use open_file, mode is "w", in env_posix.cc. So this will create log file
									 //lfile represents the log file
	//exit(1);//test if how much files have been created --.log
	//exit(1);//test for if log file being created   ---yes
	printf("_____Hello, I am DB::Open 11111,s.ok()=%d\n",s.ok());
	
    if (s.ok()) {//s.ok()
      edit.SetLogNumber(new_log_number);
      impl->logfile_ = lfile;
      impl->logfile_number_ = new_log_number;
      impl->log_ = new log::Writer(lfile);//impl->log_ now can represent the log file
	  //printf("_____Hello, I am DB::Open 11111, ,before LogAndApply, s.ok()=%d\n",s.ok());
	  //exit(1);//test if how much files have been created --.log
	  //printf("I am db_impl.cc, DB::Open, before recover, before LogAndApply\n");
      s = impl->versions_->LogAndApply(&edit, &impl->mutex_);//functin at version_set.cc line 811. This will create current, manifest
	  //printf("I am db_impl.cc, DB::Open, after recover, before LogAndApply\n");
	  //exit(1);//test if how much files have been created --current, manifest, .log
	  printf("_____Hello, I am DB::Open 11111, ,after LogAndApply, s.ok()=%d\n",s.ok());
		//exit(9);
    }
	//exit(1);//test if how much files have been created  --current, manifest, .log
    if (s.ok()) {
		 printf("ddddddd,111111\n");
      impl->DeleteObsoleteFiles();
      //impl->MaybeScheduleCompaction();
    }
  }
  //exit(1);//test if the log file is deleted --no
  impl->mutex_.Unlock();
   //printf("_____Hello, I am in db_impl, DB::Open ,s.ok()=%d\n",s.ok());
  if (s.ok()) {
    *dbptr = impl;
  } else {
    delete impl;
  }
  //printf("<<<<<<<<<<<<<<<<Hello, I am DB::Open ,end\n");
   //exit(1);//test if the log file is deleted --no
  return s;
}

Snapshot::~Snapshot() {
}

Status DestroyDB(const std::string& dbname, const Options& options) {
  Env* env = options.env;
  std::vector<std::string> filenames;
  // Ignore error in case directory does not exist
  env->GetChildren(dbname, &filenames);
  if (filenames.empty()) {
    return Status::OK();
  }

  FileLock* lock;
  const std::string lockname = LockFileName(dbname);
  Status result = env->LockFile(lockname, &lock);
  if (result.ok()) {
    uint64_t number;
    FileType type;
    for (size_t i = 0; i < filenames.size(); i++) {
      if (ParseFileName(filenames[i], &number, &type) &&
          type != kDBLockFile) {  // Lock file will be deleted at end
		  //printf("xxxx,11111\n");
        Status del = env->DeleteFile(dbname + "/" + filenames[i]);
        if (result.ok() && !del.ok()) {
          result = del;
        }
      }
    }
    env->UnlockFile(lock);  // Ignore error since state is already gone
     //printf("xxxx,2222\n");
	env->DeleteFile(lockname);
    env->DeleteDir(dbname);  // Ignore error in case dir contains other files
  }
  return result;
}

}  // namespace leveldb
