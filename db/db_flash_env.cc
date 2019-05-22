#include <stdlib.h>
#include <stdio.h>

#include "db/db_flash.h"
#include "db/filename.h"

#include <fcntl.h>
#include <sys/mman.h>

#include <time.h>
#include <stdint.h>
#define s_to_ns 1000000000
extern double durations[];//durations[1]=Write_duration

extern char * SegTable;// declare in db_flash_tools.cc

uint64_t removeNum;
uint64_t openNum;
namespace leveldb{
int Flash::rename(const std::string&src, const std::string&target){

	uint64_t target_entry_offset=get_seg_offset_by_name(target);
	uint64_t src_entry_offset=get_seg_offset_by_name(src);
	if(target_entry_offset==src_entry_offset){// names are the same 
		
		return 0;
	}
	uint64_t segment_offset=*(uint64_t*) ( dev+  src_entry_offset*SEGMENT_BYTES  + FILE_NAME_BYTES );
	std::string target_short= get_file_name(target);
	//int res=create_entry(target_short,target_entry_offset,0);//first create entry
	//copy the source entry content to the target entry --begin
	printf("db_flash_env.cc, rename, copy segment!!!src=%s, target=%s\n",src.c_str(),target.c_str());
	memcpy( dev+ target_entry_offset*SEGMENT_BYTES ,  dev+ src_entry_offset*SEGMENT_BYTES  , BLOCK_BYTES);
	memset(dev+ target_entry_offset*SEGMENT_BYTES,0,FILE_NAME_BYTES);
	memcpy( dev+ target_entry_offset*SEGMENT_BYTES , target_short.c_str()   , strlen(target_short.c_str()));
	//copy the source entry content to the target entry --end
	
	
	//if(res==0){//occupied.  Is it?
		//delete_entry(target_short,0);
		//create_entry(target_short,target_entry_offset,segment_offset);
	//}
	delete_entry(src,0);
	
	return 0;
	
}
int Flash::remove(const std::string& fname){
	
	//std::string file_name=get_file_name(fname);
	
	f_filenames.erase(fname);
	
	uint64_t seg_offset=get_seg_offset_by_name(fname);
	//printf("I am Flash::remove ,filename=%s,seg_offset=%d\n",fname.c_str(),seg_offset);
	
	removeNum++;
	SegTable[seg_offset]=0;
	//printf("I am Flash::remove ,filename=%s,seg_offset=%d\n",fname.c_str(),seg_offset);
	char * file_addr = dev+ seg_offset*BLOCK_BYTES;
	int rs=madvise(file_addr, BLOCK_BYTES, MADV_DONTNEED   );
	//int r2=posix_fadvise(dev_fd, seg_offset*BLOCK_BYTES, BLOCK_BYTES, POSIX_FADV_DONTNEED);
	//printf("I am Flash::remove , rs=%d,r2=%d\n",rs,r2);
	return 0;
	
}
int Flash::delete_entry(const std::string& filename, int mode){
	
	//printf("delete begein\n");
	uint64_t entry_offset=get_seg_offset_by_name(filename);
	
	uint64_t segment_offset=entry_offset;//*(uint64_t*)( dev+ PAGE_BYTES+entry_offset*ENTRY_BYTES +FILE_NAME_BYTES );
	
	//*(uint64_t*)(dev+ segment_offset*SEGMENT_BYTES)=0;
	//printf("delete end\n");
	return 0;
	
}
int Flash::create_entry(const std::string& fname,uint64_t entry_offset, uint64_t segment_offset){
	//this is short name
	//printf("create_entry begin,fname=%s \n",fname.c_str());
	
	printf("enter create_entry,fname=%s, exit\n",fname.c_str());
	exit(9);
	
	
	if(entry_offset==0){
		entry_offset=get_seg_offset_by_name(fname);	
	}

	segment_offset=entry_offset;
	char* entry=dev+entry_offset*(SEGMENT_BYTES);
	
	memset(entry,0,ENTRY_BYTES);
	memcpy(entry, fname.c_str(), fname.length() );	//copy the file name to entry
	//*(uint64_t *)( entry+FILE_NAME_BYTES )=segment_offset;//give the segment_offset to entry
	//(*(uint32_t *)( dev)) ++; //increase the total file number
	
	//printf("create_entry end\n");
	return segment_offset;
	
}
int Flash::access(const std::string& fname, int flag){//on success which the file exists 0 is returned, otherwise -1 is returned. 
	
	//printf("access begin\n");
	std::string file_name=get_file_name(fname);
		
	//uint64_t entry_total=(BLOCK_BYTES-PAGE_BYTES)/(ENTRY_BYTES);
	
	int entry_offset=get_seg_offset_by_name(fname);
	
	int res;
	//printf("dbflashenv, access,file_name.c_str()=%s,  entry_offset=%d\n",file_name.c_str(),entry_offset);
	//printf("dev=%p, in dev name=%s\n",dev,dev+entry_offset*(SEGMENT_BYTES) );
	if( strncmp(file_name.c_str(),this->dev+entry_offset*(SEGMENT_BYTES) , file_name.length() )  ==0){//exist				
		res= 0; //0 says the file exists
	}
	else {
		res=-1;
	}
	//printf("I am Flash::access, %s-I didn't find it!!!\n",file_name.c_str());
	
	//printf("access end\n");
	return res;// -1 says not exits// 2015.5.7 change to zero  //5.12 changed to -1
}
	//#define OPEN_FILE_ARG const std::string& fname

/*
Mode w: Create an empty file for output operations.If a file with the same name already exists, 
its contents are discarded and the file is treated as a new empty file.

First search the file, if exists, empty its data block and return the addr, 
		if not, add and entry in file name block and return the data block addr.

*/
#define OPEN_FILE_ARG const std::string& fname, const char * mode
Flash_file * Flash::open_file(OPEN_FILE_ARG){
	
	//printf("open_file begin, fname=%s, this->fd=%d\n",fname.c_str(),dev_fd);
	
	if(fname.find("LOG")!=-1){

		printf("____ hello, I am Flash::open_file, fname=%s \n",fname.c_str());
		//exit(9);
	}
	int fd;
	if(fname.find(".logg")!=-1){
		fd=open(db_name.c_str(), O_DIRECT|O_RDWR|O_SYNC);//db_name.c_str()
		//printf("____ hello, I am Flash::open_file, fname=%s \n",fname.c_str());
		//void *wbuf;
		//posix_memalign(&wbuf,512,size);
		//wbuf=malloc(512);
		//int ws=write(fd,wbuf,512);
		//exit(9);
	}
	else{
		fd=this->dev_fd;
	}
	//fd=open(db_name.c_str(),O_RDWR);
	//printf("db_flash_env, open_file, file =%s, fd=%d\n",fname.c_str(), fd);
	if(fd<0){
		printf("db_flash_env.cc, open failed, exit\n");
		printf("file =%s, fd=%d\n",fname.c_str(), fd);
		exit(9);
	}
	Flash_file *f_file=new Flash_file;
	
	std::string file_name=get_file_name(fname);
	
	//access the file to learn whether the file exist  --begin
	int rt;
	//uint64_t addr_offset;
	uint64_t seg_offset;

	//rt=this->access(file_name,0); //0 says the file exists, -1 says not exist
	//printf("____ hello, I am Flash::open_file, rt=%d \n",rt);
	seg_offset=get_seg_offset_by_name(fname);
	
	SegTable[seg_offset]=1;//make the segment non-allocatable 
	openNum++;
	//printf("db_flash_env.cc, open, open=%llu, remove=%llu\n",openNum ,removeNum);
	
	f_file->entry_addr= dev+ seg_offset*BLOCK_BYTES;//file_name_begin + get_seg_offset_by_name(fname)*ENTRY_BYTES;
	f_file->addr=f_file->entry_addr;// +ENTRY_BYTES;
	if(strcmp(mode,"r")==0 || strcmp(mode,"r+")==0){
		//printf("Attention! This is not implement. I am Flash::open_file,mode=%s \n",mode);
	}
	else if(strcmp(mode,"w")==0 || strcmp(mode,"w+")==0){
		//printf("I am Flash::open_file,mode=%s , fname=%s\n",mode,fname.c_str());
		 f_filenames.insert( get_file_name(fname) );
		 //f_handles.push_back(fd);
		//printf("I am Flash::open_file,mode=%s , fname=%s, f_filenames.size()=%d\n",mode,fname.c_str(),f_filenames.size());
		//printf("this flash=%p\n",this);
		//if(rt!=0) {
			//this->delete_entry(fname,0);// 
		//}
		memcpy(f_file->buffer, file_name.c_str(), file_name.length() );
	}
	else {
		printf("db_flash_env.cc, open not for 'w', exit\n");
		exit(9);
	}
	if(strcmp(mode,"a")==0 || strcmp(mode,"a+")==0){
		//printf("Attention! his is not implement. I am Flash::open_file,mode=%s \n",mode);
	}
		//printf("I am Flash::open_file, total_files=%d\n",total_files);
	//if(rt==0){//file exist, the rt will be the file content segment offset
			//seg_offset=get_seg_offset_by_name(fname);
			//addr_offset=*(uint64_t*)( this->dev+entry_offset*(SEGMENT_BYTES)+FILE_NAME_BYTES );
					//printf("in open file, rt=0,qqqq\n");
		//f_file->content_bytes=* (uint64_t*)( f_file->entry_addr+FILE_NAME_BYTES+FILE_ADDR_BYTES);
		//printf("in open file, rt=0,f_file->content_bytes=%x\n",f_file->content_bytes);
	
	//}
	//else{//file not exist
		
		//printf("I am Flash::open_file, file not exist, now create entry,file_name=%s, f_file->addr=%p\n",file_name.c_str(),f_file->addr);
		//seg_offset=this->create_entry(file_name,0,0);// create_entry will return the segment offset		
	
	 		
	//}

	
	//f_file->offset_bytes=0;//every time open a file, its offset should be set to the beginning
	
	f_file->file_name=fname;//this is for test the external read/write functions
	f_file->seg_offset=seg_offset;
	f_file->dev_fd=fd;
	//printf("____ hello, I am Flash::open_file, end, fname=%s,seg_offset=%d \n",fname.c_str(),f_file->seg_offset);
	//memcpy(f_file->buffer, f_file->entry_addr, ENTRY_BYTES);//copy the entry to buffer
	//f_file->w_buffer=f_file->buffer+ ENTRY_BYTES;
	
	
	//printf("open_file end\n");
	return f_file;
	
	
	
}

}//end leveldb