#include <stdlib.h>
#include <stdio.h>

#include "db/db_flash.h"
#include "db/filename.h"

#include <time.h>
#include <stdint.h>
#define s_to_ns 1000000000
extern double durations[];////durations[1]=Write_duration

char *SegTable;
uint64_t SegTotal;
uint64_t LastAlloc;
int round=0;

namespace leveldb{

int GetFreeSegNum(){
	//printf("db_flash_tools.c,GetFreeSegNum,SegTotal=%d \n",SegTotal);
	int i;
	int r=0;
	for(i=0;i<SegTotal;i++){
		//printf("%d",SegTable[i]);
		if(SegTable[i]==0) r++;
	}

	return r;
}
uint64_t AllocSeg(uint64_t next_file_number_){
	//printf("db_flash_tools.c next_file_number_=%d\n",next_file_number_);
	int freenum=GetFreeSegNum();
	int used=SegTotal-freenum;
	double ratio=( (double)used )/(double) SegTotal;
	//printf("db_flash_tools.c, used_num=%d, free_seg_num=%d, free_ratio=%f\%, LastAlloc=%d,round=%d\n",used,freenum, ratio*100,LastAlloc,round);
	//exit(0);
	uint64_t result;;
	result=next_file_number_%(SegTotal - NON_LDB_ENTRY_NUM) + NON_LDB_ENTRY_NUM; //NON_LDB_ENTRY_NUM defined in db_flash.h, now is 32
	
	uint64_t final_number;
	// if(SegTable[result]==0){//direct map
		// final_number= next_file_number_;
		// return final_number;
	// }
	// else{//step map
		uint64_t temp;
		int flag=0;
		for(temp=result; temp< SegTotal; temp++){//find to the end
			//printf("db_flash_tools.cc, find\n")
			
			if(SegTable[temp]==0){//it is free
				final_number= next_file_number_ + (temp-result);//reverse map
				if(flag>100){
					printf("db_flash_tools.cc, found in step,next_file_number_=%d result=%d temp=%d, final_number=%d, flag=%d\n",next_file_number_,result,temp,final_number,flag);					
				}		

				LastAlloc=temp;
				return final_number;
			}
			flag++;
		}
		
		flag=0;
		
		printf("db_flash_tools.cc, round back\n");
		round++;
		for(temp= NON_LDB_ENTRY_NUM ;temp<result;temp++){ //round back
			
			
			if(SegTable[temp]==0){
				final_number= next_file_number_ + (SegTotal- result) + (temp - NON_LDB_ENTRY_NUM);
				if(flag>100){		
					printf("db_flash_tools.cc, found in roundback, next_file_number_=%d, SegTotal=%d, result=%d temp=%d,flag=%d\n",next_file_number_,SegTotal,result,temp,flag);					
				}	
				
				LastAlloc=temp;
				return final_number;
			}
			flag++;
		}
		
		printf("db_flash_tools.cc, storage full,exit!\n");
		printf("db_flash_tools.cc, next_file_number_=%d, SegTotal=%d, result=%d temp=%d,flag=%d\n",next_file_number_,SegTotal,result,temp,flag);
		exit(0);
	//}
	
}

std::string get_file_name(const std::string& long_file_name){
	int found=long_file_name.find_last_of("/");
	//printf("%s ,%s \n",long_file_name.c_str(),long_file_name.substr(found+1).c_str());
	return long_file_name.substr(found+1);
	
}
std::string get_file_nam(const std::string& long_file_name){
	int found=long_file_name.find_last_of("/");
	//printf("%s ,%s \n",long_file_name.c_str(),long_file_name.substr(found+1).c_str());
	return long_file_name.substr(found+1);
	
}

uint64_t entry_hash(const std::string& long_file_name_local){


	uint64_t result=0;
	int i=0;
	//std::string seq=long_file_name_local.substr(0,long_file_name_local.length()-4);
	//int xx=atoi(long_file_name_local.c_str());
	//printf("xx=%d\n",xx);
	//printf("i hash begin, long_file_name_local=%s\n",long_file_name_local.c_str());

	//std::stringstream stream(long_file_name_local);
	//stream>>result;
	
	result=atoi(long_file_name_local.c_str());
	
	//printf("i hash, res=%d,segment_total=%d\n",result,segment_total);
	
	uint64_t test=result;
	static uint64_t flag=0;
	
	if(result > SegTotal){
		flag++;
	}
	
	//printf("i hash, res=%d, (this->segment_total - NON_LDB_ENTRY_NUM)=%d\n",result,(this->segment_total - NON_LDB_ENTRY_NUM));
	
	result=result%(SegTotal - NON_LDB_ENTRY_NUM);
	//printf("i hash, res=%d\n",result);
	result+=NON_LDB_ENTRY_NUM; //NON_LDB_ENTRY_NUM defined in db_flash.h, now is 32
		
	if(flag>0 && flag <2){
		printf("in db_flash_tools.cc, entry_hash, serial num to end, file=%s,result=%d,flag=%d \n", long_file_name_local.c_str(),result,flag);
		flag++;
		
	}
	return result;
	
}

uint64_t  Flash::get_seg_offset_by_name(const std::string& long_file_name){
	
	std::string long_file_name_local=long_file_name;
	//printf("in tools.c 111 long_file_name_local=%s\n",long_file_name_local.c_str());
	if(long_file_name_local.find("/dev/")!=-1){
		long_file_name_local=get_file_name(long_file_name);
		
	}
	//printf("in tools.c222  long_file_name_local=%s\n",long_file_name_local.c_str());
	//1 is CURRENT, 2 is LOG, 3 is MANIFEST-XXX, 4 is LOCK, 5 is xxx.log, 6 is Conclusion, 100 is xxx.ldb
	int type=0;
	//printf(" I am get_type_by_name, long_file_name_local=%s\n",long_file_name_local.c_str());
	uint64_t result;


	if(long_file_name_local.find(".ldb")!=-1 ){
		result= entry_hash(long_file_name_local);;
	}
	else if(long_file_name_local.find("CURRENT")!=-1 ){
		
		result= 1;
	}
	else if(long_file_name_local.find("LOG")!=-1 && long_file_name_local.find("old")==-1){
		result= 20;
	}
	else if(long_file_name_local.find("MANIFEST")!=-1 ){
		result= 12;//because this file can be big to over flow
	}
	else if(long_file_name_local.find("LOCK")!=-1 ){
		result= 3;
	}	
	else if(long_file_name_local.find(".log")!=-1 ){
		result= 8;
	}
	else if(long_file_name_local.find("Conclusion")!=-1 ){
		result= 5;
	}
	else{
		result= 6;//temp file
	}
	
	//printf(" I am db_flash_tools.cc get_entry_offset_by_name, long_file_name_local=%s, result=%llu \n",long_file_name_local.c_str(), result);
	return result;
}
	
	
	
}//leveldb