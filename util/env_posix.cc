// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <deque>
#include <set>
#include "leveldb/env.h"
#include "leveldb/slice.h"
#include "port/port.h"
#include "util/logging.h"
#include "util/mutexlock.h"
#include "util/posix_logger.h"

extern  std::string dev_name;
extern  int flash_using_exist;//0 is write 1 is read
extern uint64_t write_total;

#include <queue>
extern std::queue<leveldb::Flash_file*> * sync_queue;

uint64_t segment_total_global;
char *advise_table;
int advise_table_alloc;
char *dev_global;


#include <time.h>
#include <stdint.h>
#define s_to_ns 1000000000
extern double durations[];//durations[1]=Write_duration



namespace leveldb {

namespace {

static Status IOError(const std::string& context, int err_number) {
  return Status::IOError(context, strerror(err_number));
}

class PosixSequentialFile: public SequentialFile {
 //private:
 public:
  std::string filename_;
  FILE* file_;
	
	Flash_file *f_file;
 public:

  PosixSequentialFile(const std::string& fname, FILE* f)
      : filename_(fname), file_(f) {
		
	  }
	
	  PosixSequentialFile(const std::string& fname, Flash_file* f)
      : filename_(fname), f_file(f) {
		printf("____PosixSequentialFile,Flash_file  constructor\n");
	  }
  //virtual ~PosixSequentialFile() { fclose(file_); }
  virtual ~PosixSequentialFile() {
	 
      // Ignoring any potential errors
	  f_fclose(f_file);
	  	
		// fclose(file_);
    }

  
  //file->Read(kBufferSize=8192, &fragment, space); the string will be read into fragment
  virtual Status Read(size_t n, Slice* result, char* scratch) {//n=8192
    Status s;
	//printf("I am env_posic.cc,  PosixSequentialFile::Read, this=%p, this->f_file=%p,this->f_file->addr=%p \n", this,this->f_file,this->f_file->addr );
	//printf("I am PosixSequentialFile::Read,  file_addr=%p\n",file_addr);
	//printf("I am env_posix.cc,  PosixSequentialFile::Read begin\n");
	
	size_t r =flash_read(scratch, 1, n, f_file);//the read string will be pointed by scratch
    //size_t r = fread_unlocked(scratch, 1, n, file_);//#define fread_unlocked fread
	//printf("I am env_posix.cc,  PosixSequentialFile::Read2222, scratch=%s, r=%d\n",scratch,r);
	//printf("I am PosixSequentialFile::Read2222, scratch=%s\n",scratch);
	//exit(1);
    *result = Slice(scratch, r);
	 //*result = Slice("", r);
	//printf("I am PosixSequentialFile::Read2222, result->empty()=%d\n", result->empty());
    if (r < n) {
      if (1) {//feof(file_)
        // We leave status as ok if we hit the end of the file
      } else {
        // A partial read with an error: return a non-ok status
        s = IOError(filename_, errno);
      }
    }
    return s;
  }

  virtual Status Skip(uint64_t n) {
    if (fseek(file_, n, SEEK_CUR)) {
      return IOError(filename_, errno);
    }
    return Status::OK();
  }
};

// pread() based random-access
class PosixRandomAccessFile: public RandomAccessFile {
 private:
  std::string filename_;
  int fd_;

 public:
  PosixRandomAccessFile(const std::string& fname, int fd)
      : filename_(fname), fd_(fd) { 
	 // Flash_file *f_file;
	 printf("C PosixRandomAccessFile\n");
	 exit(9);
	  
	  }
  virtual ~PosixRandomAccessFile() { close(fd_); }

  virtual Status Read(uint64_t offset, size_t n, Slice* result,
                      char* scratch) const {
    Status s;
	printf(" Posix.cc, random read, begin\n");
	exit(9);
    ssize_t r = pread(fd_, scratch, n, static_cast<off_t>(offset));
    *result = Slice(scratch, (r < 0) ? 0 : r);
    if (r < 0) {
      // An error: return a non-ok status
      s = IOError(filename_, errno);
    }
    return s;
  }
};

// Helper class to limit mmap file usage so that we do not end up
// running out virtual memory or running into kernel performance
// problems for very large databases.
class MmapLimiter {
 public:
  // Up to 1000 mmaps for 64-bit binaries; none for smaller pointer sizes.
  MmapLimiter() {
    SetAllowed(sizeof(void*) >= 8 ? 1000 : 0);
  }

  // If another mmap slot is available, acquire it and return true.
  // Else return false.
  bool Acquire() {
    if (GetAllowed() <= 0) {
      return false;
    }
    MutexLock l(&mu_);
    intptr_t x = GetAllowed();
    if (x <= 0) {
      return false;
    } else {
      SetAllowed(x - 1);
      return true;
    }
  }

  // Release a slot acquired by a previous call to Acquire() that returned true.
  void Release() {
    MutexLock l(&mu_);
    SetAllowed(GetAllowed() + 1);
  }

 private:
  port::Mutex mu_;
  port::AtomicPointer allowed_;

  intptr_t GetAllowed() const {
    return reinterpret_cast<intptr_t>(allowed_.Acquire_Load());
  }

  // REQUIRES: mu_ must be held
  void SetAllowed(intptr_t v) {
    allowed_.Release_Store(reinterpret_cast<void*>(v));
  }

  MmapLimiter(const MmapLimiter&);
  void operator=(const MmapLimiter&);
};

// mmap() based random-access

class PosixMmapReadableFile: public RandomAccessFile {
 private:
  std::string filename_;
  void* mmapped_region_;
  size_t length_;
  MmapLimiter* limiter_;
  
  //uint64_t segment_total;

 public:
  // base[0,length-1] contains the mmapped contents of the file.
  PosixMmapReadableFile(const std::string& fname, void* base, size_t length,
                        MmapLimiter* limiter)
      : filename_(fname), mmapped_region_(base), length_(length),
        limiter_(limiter) {
		
		//printf(" C PosixMmapReadableFile, fname=%s\n",fname.c_str());
		
  }

  virtual ~PosixMmapReadableFile() {
  
	//printf("in env_posix.cc, ~PosixMmapReadableFile() \n");
   // munmap(mmapped_region_, length_);
    limiter_->Release();
  }

  virtual Status Read(uint64_t offset, size_t n, Slice* result,
                      char* scratch) const {
					

						  
    Status s;
	//printf("PosixMmapReadableFile, Read, begin,mmapped_region_=%p,offset=%d,n=%d,length_=%d\n",mmapped_region_,offset,n,length_);
	//exit(9);
	
	durations[70]+=n;
		durations[71]++;

    if (offset + n > length_) {//
		printf("env_posix.cc, PosixMmapReadableFile,Read error, overflow exit,length_=%d\n", length_);
		printf("PosixMmapReadableFile, Read, begin,filename_=%s,offset=%d,n=%d,length_=%d\n",filename_.c_str(),offset,n,length_);

		
		//exit(6);
      *result = Slice();
      s = IOError(filename_, EINVAL);
    } 
	else {
		//printf("env_posix.cc, PosixMmapReadableFile,Read realy begin\n"); 8388608
	  //printf("env_posix.cc, %s\n", reinterpret_cast<char*>(mmapped_region_) + offset );
	  
	  //printf("env_posix.cc, mmapped_region_=%x,segment_total_global=%d, mod=%d \n",(uint64_t)mmapped_region_,segment_total_global, (uint64_t)mmapped_region_% segment_total_global);
	  
	 	*result = Slice(reinterpret_cast<char*>(mmapped_region_) + offset, n);

	  //printf("region=%d,mmapped_region_=%p, region=%p\n",region,mmapped_region_ ,(void*)(region-1));
#define ADVISE_M 512
#define ADVISE_SWITCH 0
	  //if file not advised, advise
		// if(ADVISE_SWITCH!=1){
			// return s;
		// }
		// uint64_t distance = (char*)mmapped_region_  - (char*)dev_global + offset;
		// uint64_t seg_num = distance / BLOCK_BYTES;
		// uint64_t mod = seg_num *ADVISE_M + (offset + ENTRY_BYTES)/(BLOCK_BYTES/ADVISE_M);
			
		// //uint64_t mod=(uint64_t)mmapped_region_ / (segment_total_global/ADVISE_M);
		// if(advise_table[mod]==0 && ADVISE_SWITCH==1){
			
			
			// void * advise_addr = dev_global + mod*((BLOCK_BYTES/ADVISE_M));
			// madvise(reinterpret_cast<char*>(advise_addr),  BLOCK_BYTES/ADVISE_M, MADV_WILLNEED);
			// //durations[60]++;
			// advise_table[mod]=1;
		// }
		// else{
			// //printf("advise_table[mod]=%d, ADVISE_SWITCH=%d\n",advise_table[mod],ADVISE_SWITCH);
		// }
		

	  //file advised
	  
    }

	//printf("durations[50]=%f\n",durations[50]);
    return s;
  }
};

class PosixWritableFile : public WritableFile {
 public:
  std::string filename_;
  FILE* file_;
  
  Flash_file *f_file;

 public:
  PosixWritableFile(const std::string& fname, FILE* f)
      : filename_(fname), file_(f) { }
	
	  PosixWritableFile(const std::string& fname, Flash_file* f)
      : filename_(fname), f_file(f) { }
  ~PosixWritableFile() {
	  if (f_file != NULL) {
		
			//printf(" i am ~PosixWritableFile, begin\n");
      // Ignoring any potential errors
	  f_fclose(f_file);
	  	//printf(" i am ~PosixWritableFile, end\n");
		// fclose(file_);
    }
	  
    //if (file_ != NULL) {
		 
      // Ignoring any potential errors
      //fclose(file_);
   // }
  }

  void test(){
	printf("env_posix.cc,hahahah, file_size=%d,buffer_file_size=%d\n",f_file->offset_bytes-ENTRY_BYTES,*(uint64_t*)(f_file->buffer+FILE_NAME_BYTES+FILE_ADDR_BYTES)-ENTRY_BYTES);
		
  
  }
  
  virtual Status Append(const Slice& data) {
	  
	    //printf("hello, i am Env_Posix, Append\n" );
//struct timespec begin_flash_write, end_flash_write;
//clock_gettime(CLOCK_MONOTONIC,&begin_flash_write);
	
		size_t r=flash_write(data.data(), 1, data.size(), f_file);
//clock_gettime(CLOCK_MONOTONIC,&end_flash_write);
//durations[6] +=( (int)end_flash_write.tv_sec +((double)end_flash_write.tv_nsec)/s_to_ns ) - ( (int)begin_flash_write.tv_sec+((double)begin_flash_write.tv_nsec)/s_to_ns );
    //size_t r = fwrite_unlocked(data.data(), 1, data.size(), file_);
	//printf("hello, i am Env_Posix, Append,r=%d, data.size()=%d \n",r,data.size());
    if (r != data.size()) {
      return IOError(filename_, errno);
    }
	//printf("hello, i am Env_Posix,  end\n");
	write_total+=data.size();
    return Status::OK();
  }

  virtual Status Close() {
    Status result;
	
	//delete f_file;
	//printf("I am env_posix.cc, Close, file_=%p, content_bytes=%d, file_name=%s\n",f_file, f_file->content_bytes,f_file->file_name.c_str());
	 return result;//make it do nothing 
    /*
	if (fclose(file_) != 0) {
      result = IOError(filename_, errno);
    }
    file_ = NULL;
    return result;
	*/
  }

  virtual Status Flush() {
	
	  //return Status::OK();// do nothing but return
	  //printf(" I am flush, file_name=%s\n",f_file->file_name.c_str());
	  
//struct timespec begin_Flush, end_Flush;
//clock_gettime(CLOCK_MONOTONIC,&begin_Flush);

  
	  
	  //printf("env_posix, I am flush, file_name=%s\n",f_file->file_name.c_str());
	if(f_file->file_name.find(".log")!=-1){
		 //flash_flush_log(f_file);
	}
	//else{
	  //flash_flush(f_file);
	//}
	  //flash_entry_flush(f_file);
	     
//clock_gettime(CLOCK_MONOTONIC,&end_Flush);
//durations[34] +=( (int)end_Flush.tv_sec +((double)end_Flush.tv_nsec)/s_to_ns ) - ( (int)begin_Flush.tv_sec+((double)begin_Flush.tv_nsec)/s_to_ns );
	  
	  
	  return Status::OK();

   
	  /*
    if (fflush_unlocked(file_) != 0) {
      return IOError(filename_, errno);
    }
    return Status::OK();
	*/
  }

  Status SyncDirIfManifest() {
    const char* f = filename_.c_str();
    const char* sep = strrchr(f, '/');
    Slice basename;
    std::string dir;
    if (sep == NULL) {
      dir = ".";
      basename = f;
    } else {
      dir = std::string(f, sep - f);
      basename = sep + 1;
    }
    Status s;
	return s;//make it do nothing 
	
    if (basename.starts_with("MANIFEST")) {
      int fd = open(dir.c_str(), O_RDONLY);
      if (fd < 0) {
        s = IOError(dir, errno);
      } else {
        if (fsync(fd) < 0) {
          s = IOError(dir, errno);
        }
        close(fd);
      }
    }
    return s;
  }

  virtual Status Sync() {
    // Ensure new files referred to by the manifest are in the filesystem.
	//printf("I am endv_posix, Sync()\n");
	//exit(0);
durations[36]++;
struct timespec begin_Sync, end_Sync;
clock_gettime(CLOCK_MONOTONIC,&begin_Sync);	
	 Status s ;
	 
// struct timespec begin_Sync_Flush, end_Sync_Flush;
// clock_gettime(CLOCK_MONOTONIC,&begin_Sync_Flush);
	//printf("I am PosixWritableFile, Sync(),f_file=%s\n",f_file->file_name.c_str());
	if(f_file->file_name.find(".log")!=-1){
	
		flash_flush_log(f_file);
clock_gettime(CLOCK_MONOTONIC,&end_Sync);
durations[35] +=( (int)end_Sync.tv_sec +((double)end_Sync.tv_nsec)/s_to_ns ) - ( (int)begin_Sync.tv_sec+((double)begin_Sync.tv_nsec)/s_to_ns );
		return s;
	}
	
	 flash_entry_flush(f_file);
	 
	//flash_flush(f_file);//write();//fflush// use write to write file_buffer to dev
	flash_flush_tail(f_file); // use back aligned to flush the all remained buffer
// clock_gettime(CLOCK_MONOTONIC,&end_Sync_Flush);
// durations[39] +=( (int)end_Sync_Flush.tv_sec +((double)end_Sync_Flush.tv_nsec)/s_to_ns ) - ( (int)begin_Sync_Flush.tv_sec+((double)begin_Sync_Flush.tv_nsec)/s_to_ns );



  	 
	 flash_sync(f_file);
	 
	 //printf("endv_posix, sync_queue size=%d\n",sync_queue->size());
	
clock_gettime(CLOCK_MONOTONIC,&end_Sync);
durations[35] +=( (int)end_Sync.tv_sec +((double)end_Sync.tv_nsec)/s_to_ns ) - ( (int)begin_Sync.tv_sec+((double)begin_Sync.tv_nsec)/s_to_ns );

	 return s;//make it do nothing 
		/*
		s = SyncDirIfManifest();
		 
		if (!s.ok()) {
		  return s;
		}
		if (fflush_unlocked(file_) != 0 ||
			fdatasync(fileno(file_)) != 0) {
		  s = Status::IOError(filename_, strerror(errno));
		}
		return s;
		*/
  }
};

static int LockOrUnlock(int fd, bool lock) {
  errno = 0;
  struct flock f;
  memset(&f, 0, sizeof(f));
  f.l_type = (lock ? F_WRLCK : F_UNLCK);
  f.l_whence = SEEK_SET;
  f.l_start = 0;
  f.l_len = 0;        // Lock/unlock entire file
  return fcntl(fd, F_SETLK, &f);
}

class PosixFileLock : public FileLock {
 public:
  int fd_;
  std::string name_;
};

// Set of locked files.  We keep a separate set instead of just
// relying on fcntrl(F_SETLK) since fcntl(F_SETLK) does not provide
// any protection against multiple uses from the same process.
class PosixLockTable {
 private:
  port::Mutex mu_;
  std::set<std::string> locked_files_;
 public:
  bool Insert(const std::string& fname) {
    MutexLock l(&mu_);
    return locked_files_.insert(fname).second;
  }
  void Remove(const std::string& fname) {
    MutexLock l(&mu_);
    locked_files_.erase(fname);
  }
};

class PosixEnv : public Env {
 public:
  PosixEnv();
  virtual ~PosixEnv() {
    char msg[] = "Destroying Env::Default()\n";
    fwrite(msg, 1, sizeof(msg), stderr);
    abort();
  }

  virtual Status NewSequentialFile(const std::string& fname,
                                   SequentialFile** result) {
	
	printf(" I am NewSequentialFile\n");
	//first find the file block by the fname, then pass its address
		Flash_file *f_file;
		
		f_file=env_flash->open_file(fname, "r");
		//printf("I am env_posix.cc,NewSequentialFile,f_file=%p, f_file->addr=%p\n",f_file,f_file->addr);
	//then new
	
	if(f_file->addr==NULL){
		//printf("I am env_posix.cc,NewSequentialFile, f_file->addr==NULL\n");
	}
	else{
		//printf(" I am NewSequentialFile, OK\n");
		*result = new PosixSequentialFile(fname, f_file);
		//PosixSequentialFile* x=(PosixSequentialFile*)*result;
		
		//printf("I am env_posix.cc,NewSequentialFile,*result=%p,x=%p, x->f_file=%p, x->f_file->addr=%p\n",*result,x,x->f_file,x->f_file->addr);
		//exit(1);
		return Status::OK();
	}
	//exit(1);
	/*
    FILE* f = fopen(fname.c_str(), "r");
    if (f == NULL) {
      *result = NULL;
      return IOError(fname, errno);
    } else {
      *result = new PosixSequentialFile(fname, f);
      return Status::OK();
    }
	*/
  }

  virtual Status NewRandomAccessFile(const std::string& fname_,
                                     RandomAccessFile** result) {

						 
    *result = NULL;
    Status s;
	uint64_t bytes_offset;
	std::string fname=fname_;
	//printf("I am env_posix.cc, NewRandomAccessFile, begin, fanme=%s \n",fname.c_str());

	//now i can directly get the entry data of the file, but I need re-mmap according to the file size so it keeps comply with "void * base"
	//printf("in env_posix.cc, NewRandomAccessFile, before get_seg_offset_by_name\n");
	uint64_t entry_offset=env_flash->get_seg_offset_by_name(fname);
	//bytes_offset= (*(uint64_t *) (env_flash->dev+ (SEGMENT_BYTES)*entry_offset + FILE_NAME_BYTES) ) *BLOCK_BYTES ;
	//printf("%llu bytes_offset=%llu \n",entry_offset,bytes_offset);

	//dev+PAGE_BYTES+ ENTRY_BYTES*entry_offset + FILE_NAME_BYTES   ;this is file segment offset 
	//dev+PAGE_BYTES+ ENTRY_BYTES*entry_offset + FILE_NAME_BYTES + FILE_ADDR_BYTES ; this is file lenght
	//fname=fname.substr(0,8);
	//printf("%s \n",fname.substr(0,8).c_str());
	//exit(1);

	//int fd = open(fname.substr(0,8).c_str(), O_RDONLY);//use raw open for mmap, here should use the flash_open which directly give the mmaped result
    int fd=3;
	//printf("fd=%d\n",fd);
	if (fd < 0) {
      s = IOError(fname, errno);
    } else if (mmap_limit_.Acquire()) {
      uint64_t size;
	  
	  //size=8388608;
      s = GetFileSize(fname, &size);//get the file size, here we can directly read the size from the mmaped file above
	  //printf("NewRandomAccessFile, fname=%s, GetFileSize=%d\n",fname.c_str(),size);
      if (s.ok()) {
        //bytes_offset=(SEGMENT_BYTES)*entry_offset + ENTRY_BYTES;
		//void* base = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, bytes_offset);//mmap the file and give the address to base
		void *base=   env_flash->dev+ (SEGMENT_BYTES)*entry_offset + ENTRY_BYTES;	
		
		
		
		if(advise_table_alloc==0){
			segment_total_global= env_flash->segment_total;
			dev_global= env_flash->dev;
			
			uint64_t table_size= env_flash->segment_total * ADVISE_M;
			printf("env_posix.cc, alloc memory for advise table, table size=%d\n",table_size);

			advise_table = (char*) malloc(table_size);
			memset(advise_table,0,table_size);
			advise_table_alloc=1;
			
		}
		//printf("base=%p\n",base);
	   if (base != MAP_FAILED) {
          *result = new PosixMmapReadableFile(fname, base, size, &mmap_limit_);// create a mmaped file, this is the port for translating
        } else {
          s = IOError(fname, errno);
        }
      }
      //close(fd);
      if (!s.ok()) {
        mmap_limit_.Release();
      }
    } else {
		printf("in NewRandomAccessFile, need fd,exit\n");
		exit(9);
      //*result = new PosixRandomAccessFile(fname, fd);// give the result file for access
    }

	//printf("in env_posix.cc, NewRandomAccessFile, end get_seg_offset_by_name\n");
    return s;
	
	
	/*
    int fd = open(fname.c_str(), O_RDONLY);//use raw open for mmap, here should use the flash_open which directly give the mmaped result
    if (fd < 0) {
      s = IOError(fname, errno);
    } else if (mmap_limit_.Acquire()) {
      uint64_t size;
      s = GetFileSize(fname, &size);//get the file size, here we can directly read the size from the mmaped file above
      if (s.ok()) {
        void* base = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);//mmap the file and give the address to base
        if (base != MAP_FAILED) {
          *result = new PosixMmapReadableFile(fname, base, size, &mmap_limit_);// create a mmaped file, this is the port for translating
        } else {
          s = IOError(fname, errno);
        }
      }
      close(fd);
      if (!s.ok()) {
        mmap_limit_.Release();
      }
    } else {
      *result = new PosixRandomAccessFile(fname, fd);// give the result file for access
    }
    return s;
	*/
  }

  virtual Status NewWritableFile(const std::string& fname,
                                 WritableFile** result) {
    Status s;
	
	//printf(" hello, I am NewWritableFile\n");
	//printf(" hello, I am NewWritableFile, fname=%s\n",fname.c_str());
	//exit(1);//test where the deleting happens
    //char *f=flash_fopen(fname.c_str(), "w");
//struct timespec begin_open_file, end_open_file;
//clock_gettime(CLOCK_MONOTONIC,&begin_open_file);
	  
	
	Flash_file *f=env_flash->open_file(fname,"w");

//clock_gettime(CLOCK_MONOTONIC,&end_open_file);
//durations[27] +=( (int)end_open_file.tv_sec +((double)end_open_file.tv_nsec)/s_to_ns ) - ( (int)begin_open_file.tv_sec+((double)begin_open_file.tv_nsec)/s_to_ns );
	  	
	//printf(" hello, I am NewWritableFile, f->addr=%p\n",f->addr);
	//printf(" hello, I am NewWritableFile, f->addr=%p\n",(env_flash->open_file(fname,"w"))->addr);
	//exit(1);
	//FILE* f = fopen(fname.c_str(), "w");
    if (f == NULL) {
      *result = NULL;
      s = IOError(fname, errno);
    } else {
      *result = new PosixWritableFile(fname, f);
    }
	//printf(" *****hello, I am NewWritableFile, f->addr=%p ,s.ok()=%d\n",f->addr,s.ok());
    return s;
  }

  virtual bool FileExists(const std::string& fname) {
	  
	//printf(" Iam FileExists\n");
	//printf(" Iam FileExists env_flash->dev=%p\n",env_flash->dev);
	
	printf("envposix,fileexits,env_flash->f_filenames.size()=%s\n",env_flash->f_filenames.size());
	//exit(1);
	return 1;
    //return env_flash->access(fname.c_str(), F_OK) != -1;
	// return access(fname.c_str(), F_OK) ==0;
  }

  virtual Status GetChildren(const std::string& dir,
                             std::vector<std::string>* result) {
    result->clear(); //result contains strings 
	
	//printf("_________I am env_posix.cc GetChildren, dir=%s, env_flash->dev=%p\n",dir.c_str(),env_flash->dev);
	
	//printf("filenames.size=%d, filehandles.size=%d\n", env_flash->f_filenames.size(),env_flash->f_handles.size());
	
	//*result=(env_flash->f_filenames);
	 for (std::set<std::string>::iterator it=env_flash->f_filenames.begin(); it!=env_flash->f_filenames.end(); ++it){
		result->push_back(*it);
	 }
	int i;
	//for(std::list<int>::iterator it=env_flash->f_filenames.begin(); it != env_flash->f_filenames.end(); ++it){
	//	result->push_back(*it);
	//}
	return Status::OK();
	
	/*
	//struct dirent* flash_entry;
	uint64_t advanced;
	//return Status::OK();
	
	//char *entries_begin=env_flash->dev + PAGE_BYTES;
	//char *file_name;
	
	//printf("I am env_posic.cc --------begin \n ");
	for(advanced=PAGE_BYTES;advanced<BLOCK_BYTES; advanced=advanced+ENTRY_BYTES){
		file_name=env_flash->dev + advanced;
		
		if(*file_name!=0){
			printf("env_posix.cc, GetChildren, file_name=%s ", file_name);
			result->push_back(file_name);
			///printf("I am env_posix.cc GetChildren, file_name=%s\n",file_name);
		}
	}
	 //printf("\n--------_________I am env_posix.cc GetChildren, end \n ");
	 */
	
	
	//below are the original code
	/*
    DIR* d = opendir(dir.c_str());
    if (d == NULL) {
      return IOError(dir, errno);
    }
    struct dirent* entry;
    while ((entry = readdir(d)) != NULL) {
      result->push_back(entry->d_name);
    }
    closedir(d);
    return Status::OK();
	*/
  }

  virtual Status DeleteFile(const std::string& fname) {

	 // struct timespec begin_erase, end_erase;
//clock_gettime(CLOCK_MONOTONIC,&begin_erase);
		
	//printf(" i am env_posix.cc, DeleteFile, fname=%s,f_filenames=%d\n",fname.c_str(), env_flash->f_filenames.size());
	//printf("env_flash=%p\n",env_flash);
	int i;
	//for(i=0;i<env_flash->f_filenames.size();i++){
		//printf("in deletefile, file=%s, todeleteis=%s\n",env_flash->f_filenames[i].c_str(),fname.c_str());
		//if(fname.find(env_flash->f_filenames[i])!=-1){
			//printf("in delete, has,fsize=%d,hsize=%d\n", env_flash->f_filenames.size(), env_flash->f_handles.size());
	
			//close(env_flash->f_handles[i]);//the very point
			
			//env_flash->f_filenames.erase(env_flash->f_filenames.begin()+i);
			//env_flash->f_handles.erase(env_flash->f_handles.begin()+i);
			
			
			
			//exit(9);
		//}
	//}
	
	env_flash->remove(fname);
//clock_gettime(CLOCK_MONOTONIC,&end_erase);
//durations[40] +=( (int)end_erase.tv_sec +((double)end_erase.tv_nsec)/s_to_ns ) - ( (int)begin_erase.tv_sec+((double)begin_erase.tv_nsec)/s_to_ns );

	//exit(9);
    Status result;
	//return result;//have not implemented
	if (env_flash->delete_entry(fname,0) != 0) {
   // if (unlink(fname.c_str()) != 0) {
	  printf("DeleteFile error!\n");
      result = IOError(fname, errno);
    }
    return result;
  }

  virtual Status CreateDir(const std::string& name) {
	  
	  // printf(" i am env_posix.cc, CreateDir, name=%s\n",name.c_str());
    Status result;
	
	//printf(" Iam CreateDir name=%s\n",name.c_str());
	//exit(1);
	//printf("Iam CreateDir, dev=%p\n",env_flash->dev);
	//Flash * flash_=new Flash;
	//int is_format=env_flash->flash_init(name);
	//printf("Iam CreateDir, is_format=%d\n",is_format);
	//if(!is_format){
	//	env_flash->flash_format(name);
	//}
	//env_flash->flash_format(name);//for test, always format at every starting up
	//env_flash->flash_open(name);
	//printf("Iam CreateDir, dev=%p\n",env_flash->dev );
	
    //if (mkdir(name.c_str(), 0755) != 0) {
      //result = IOError(name, errno);
    //}
	
	//exit(1);
    return result;
  }

  virtual Status DeleteDir(const std::string& name) {
	  // printf(" i am env_posix.cc, DeleteDir, name=%s\n",name.c_str());
    Status result;//
	return result;//have not implemented
    if (rmdir(name.c_str()) != 0) {
      result = IOError(name, errno);
    }
    return result;
  }

  virtual Status GetFileSize(const std::string& fname, uint64_t* size) {

	 
	  // printf(" i am env_posix.cc, GetFileSize, fname=%s\n",fname.c_str());
    Status s;
	env_flash->dev_fd;
	uint64_t entry_offset = env_flash->get_seg_offset_by_name(fname);
	char r_buffer[ENTRY_BYTES];
	//memset(r_buffer,0,ENTRY_BYTES);
	//lseek(env_flash->dev_fd, entry_offset *(SEGMENT_BYTES)  ,  SEEK_SET);
	memcpy(r_buffer, env_flash->dev + entry_offset *(SEGMENT_BYTES), ENTRY_BYTES);
	//int res= read(env_flash->dev_fd, r_buffer,ENTRY_BYTES);
	//printf("in GetFileSie,res=%d, r_buffer=%s,env_flash->dev_fd=%d, entry_offset=%d\n",res,r_buffer,env_flash->dev_fd ,entry_offset);
	//printf("r_buffer+FILE_NAME_BYTES + FILE_ADDR_BYTES=%x\n",*(r_buffer+FILE_NAME_BYTES + FILE_ADDR_BYTES));
	
	*size=*(uint64_t *)(r_buffer+FILE_NAME_BYTES + FILE_ADDR_BYTES);

	*size =SEGMENT_BYTES;
	//(env_flash->dev +  entry_offset*(SEGMENT_BYTES) + FILE_NAME_BYTES + FILE_ADDR_BYTES   )  -ENTRY_BYTES;
	return s;
	/*
    struct stat sbuf;
    if (stat(fname.c_str(), &sbuf) != 0) {
      *size = 0;
      s = IOError(fname, errno);
    } else {
      *size = sbuf.st_size;
    }
    return s;
	*/
  }

  virtual Status RenameFile(const std::string& src, const std::string& target) {
	   //printf(" i am env_posix.cc, RenameFile, src=%s,target=%s\n",src.c_str(),target.c_str());
    Status result;
	//return result;//have not implemented
	if (env_flash->rename(src, target) != 0) {
	
   // if (rename(src.c_str(), target.c_str()) != 0) {
      result = IOError(src, errno);
    }
    return result;
  }

  virtual Status LockFile(const std::string& fname, FileLock** lock) {
    *lock = NULL;
    Status result;
	 return result;//skip this function
    int fd = open(fname.c_str(), O_RDWR | O_CREAT, 0644);
    if (fd < 0) {
      result = IOError(fname, errno);
    } else if (!locks_.Insert(fname)) {
      close(fd);
      result = Status::IOError("lock " + fname, "already held by process");
    } else if (LockOrUnlock(fd, true) == -1) {
      result = IOError("lock " + fname, errno);
      close(fd);
      locks_.Remove(fname);
    } else {
      PosixFileLock* my_lock = new PosixFileLock;
      my_lock->fd_ = fd;
      my_lock->name_ = fname;
      *lock = my_lock;
    }
    return result;
  }

  virtual Status UnlockFile(FileLock* lock) {
    PosixFileLock* my_lock = reinterpret_cast<PosixFileLock*>(lock);
    Status result;
	return result;//have not implemented
    if (LockOrUnlock(my_lock->fd_, false) == -1) {
      result = IOError("unlock", errno);
    }
    locks_.Remove(my_lock->name_);
    close(my_lock->fd_);
    delete my_lock;
    return result;
  }

  virtual void Schedule(void (*function)(void*), void* arg);

  virtual void StartThread(void (*function)(void* arg), void* arg);

  virtual Status GetTestDirectory(std::string* result) {
    const char* env = getenv("TEST_TMPDIR");
    if (env && env[0] != '\0') {
      *result = env;
    } else {
      char buf[100];
      snprintf(buf, sizeof(buf), "/tmp/leveldbtest-%d", int(geteuid()));
      *result = buf;
    }
    // Directory may already exist
    CreateDir(*result);
    return Status::OK();
  }

  static uint64_t gettid() {
    pthread_t tid = pthread_self();
    uint64_t thread_id = 0;
    memcpy(&thread_id, &tid, std::min(sizeof(thread_id), sizeof(tid)));
    return thread_id;
  }

  virtual Status NewLogger(const std::string& fname, Logger** result) {
  printf("envposix,Newlogger, fname=%s\n",fname.c_str());
    FILE* f = fopen(fname.c_str(), "w");
	//Flash_file *f = env_flash->open_file(fname.c_str(), "w");
    if (f == NULL) {
		printf("envposix,Newlogger, f is NULL\n");
      *result = NULL;
      return IOError(fname, errno);
    } else {
      *result = new PosixLogger(f, &PosixEnv::gettid);
      return Status::OK();
    }
  }

  virtual uint64_t NowMicros() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return static_cast<uint64_t>(tv.tv_sec) * 1000000 + tv.tv_usec;
  }

  virtual void SleepForMicroseconds(int micros) {
    usleep(micros);
  }

 private:
  void PthreadCall(const char* label, int result) {
    if (result != 0) {
      fprintf(stderr, "pthread %s: %s\n", label, strerror(result));
      abort();
    }
  }

  // BGThread() is the body of the background thread
  void BGThread();
  static void* BGThreadWrapper(void* arg) {
    reinterpret_cast<PosixEnv*>(arg)->BGThread();
    return NULL;
  }

  pthread_mutex_t mu_;
  pthread_cond_t bgsignal_;
  pthread_t bgthread_;
  bool started_bgthread_;

  // Entry per Schedule() call
  struct BGItem { void* arg; void (*function)(void*); };
  typedef std::deque<BGItem> BGQueue;
  BGQueue queue_;

  PosixLockTable locks_;
  MmapLimiter mmap_limit_;
};

PosixEnv::PosixEnv() : started_bgthread_(false) {
	
	printf("Iam env_posix.cc, PosixEnv(), begin\n");
	env_flash=new Flash;
	  
	// init the device  and open it --begin 
	 std::string name=dev_name;
	 int init_rt=env_flash->flash_init(name);
	printf("Iam env_posix.cc, PosixEnv, init_rt=%d\n",init_rt);
		if(init_rt==3){
			printf("In env_posix constructor, init_rt=3, now format\n");
			env_flash->flash_format(name);
		}
	if(flash_using_exist==0){
		printf("In env_posix constructor, flash_using_exist=0, now format\n");
		env_flash->flash_format(name);//for test, always format at every starting up
	}
		env_flash->flash_open(name);
	// init the device  and open it --end 
	
  PthreadCall("mutex_init", pthread_mutex_init(&mu_, NULL));
  PthreadCall("cvar_init", pthread_cond_init(&bgsignal_, NULL));
}

void PosixEnv::Schedule(void (*function)(void*), void* arg) {
  PthreadCall("lock", pthread_mutex_lock(&mu_));

  // Start background thread if necessary
  if (!started_bgthread_) {
    started_bgthread_ = true;
    PthreadCall(
        "create thread",
        pthread_create(&bgthread_, NULL,  &PosixEnv::BGThreadWrapper, this));
  }

  // If the queue is currently empty, the background thread may currently be
  // waiting.
  if (queue_.empty()) {
    PthreadCall("signal", pthread_cond_signal(&bgsignal_));
  }

  // Add to priority queue
  queue_.push_back(BGItem());
  queue_.back().function = function;
  queue_.back().arg = arg;

  PthreadCall("unlock", pthread_mutex_unlock(&mu_));
}

void PosixEnv::BGThread() {
  while (true) {
    // Wait until there is an item that is ready to run
    PthreadCall("lock", pthread_mutex_lock(&mu_));
    while (queue_.empty()) {
      PthreadCall("wait", pthread_cond_wait(&bgsignal_, &mu_));
    }

    void (*function)(void*) = queue_.front().function;
    void* arg = queue_.front().arg;
    queue_.pop_front();

    PthreadCall("unlock", pthread_mutex_unlock(&mu_));
    (*function)(arg);
  }
}

namespace {
struct StartThreadState {
  void (*user_function)(void*);
  void* arg;
};
}
static void* StartThreadWrapper(void* arg) {
  StartThreadState* state = reinterpret_cast<StartThreadState*>(arg);
  state->user_function(state->arg);
  delete state;
  return NULL;
}

void PosixEnv::StartThread(void (*function)(void* arg), void* arg) {
  pthread_t t;
  StartThreadState* state = new StartThreadState;
  state->user_function = function;
  state->arg = arg;
  PthreadCall("start thread",
              pthread_create(&t, NULL,  &StartThreadWrapper, state));
}

}  // namespace

static pthread_once_t once = PTHREAD_ONCE_INIT;
static Env* default_env;
static void InitDefaultEnv() { default_env = new PosixEnv; }

// Env* Env::Default() {
  // pthread_once(&once, InitDefaultEnv);
  // return default_env;
// }

}  // namespace leveldb
