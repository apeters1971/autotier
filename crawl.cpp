/*
    Copyright (C) 2019 Joshua Boudreau
    
    This file is part of autotier.

    autotier is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    autotier is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with autotier.  If not, see <https://www.gnu.org/licenses/>.
*/

#include "crawl.hpp"
#include "config.hpp"
#include "alert.hpp"
#include "xxhash64.h"
#include <iomanip>
#include <regex>
#include <pwd.h>
#include <grp.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/statvfs.h>
#include <utime.h>
#include <unistd.h>
#include <fcntl.h>
#include <list>

Tier *highest_tier = NULL;
Tier *lowest_tier = NULL;

void launch_crawlers(){
  Log("autotier started.\n",1);
  // get ordered list of files in each tier
  for(Tier *tptr = highest_tier; tptr != NULL; tptr=tptr->lower){
      tptr->crawl(tptr->dir);
  }
  
  if(config.log_lvl >= 2) dump_tiers();
  
  // tier down
  for(Tier *tptr = highest_tier; tptr->lower != NULL; tptr=tptr->lower){
    while(!tptr->files.empty() && get_fs_usage(tptr->dir) >= tptr->max_watermark){
      if(tptr->files.back().pinned_to.empty() || tptr->files.back().pinned_to != tptr->dir){
        tptr->tier_down(tptr->files.back());
      }else{
        tptr->files.pop_back(); // skip pinned files
      }
    }
  }
  // tier up
  for(Tier *tptr = lowest_tier; tptr->higher != NULL; tptr=tptr->higher){
    while(!tptr->files.empty() && get_fs_usage(tptr->higher->dir, &(tptr->files.front())) < tptr->higher->min_watermark){
      if(tptr->files.front().pinned_to.empty() || tptr->files.front().pinned_to != tptr->dir){
        tptr->tier_up(tptr->files.front());
      }else{
        tptr->files.pop_front(); // skip pinned files
      }
    }
  }
  Log("Tiering complete.\n",1);
}

void Tier::crawl(fs::path dir){
  Log("Gathering file list.",2);
  for(fs::directory_iterator itr{dir}; itr != fs::directory_iterator{}; *itr++){
    if(is_directory(*itr)){
      this->crawl(*itr);
    }else if(!is_symlink(*itr) &&
    !regex_match((*itr).path().filename().string(), std::regex("(^\\..*(\\.swp)$|^(\\.~lock\\.).*#$|^(~\\$))"))){
      this->files.push_back(File{*itr});
    }
  }
  Log("Sorting files.\n",2);
  this->files.sort(
    [](const File &a, const File &b){
      return (a.priority == b.priority)? a.times.actime > b.times.actime : a.priority > b.priority;
    }
  );
}

void Tier::tier_down(File &file){
  Log("Tiering down.",2);
  fs::path to_here = this->lower->dir/relative(file.path, this->dir);
  if(!is_directory(to_here.parent_path()))
    create_directories(to_here.parent_path());
  Log("Copying " + file.path.string() + " to " + to_here.string(),2);
  copy_file(file.path, to_here); // move item to slow tier
  copy_ownership_and_perms(file.path, to_here);
  if(verify_copy(file.path, to_here)){
    Log("Copy succeeded.",2);
    remove(file.path);
    create_symlink(to_here, file.path); // create symlink fast/item -> slow/item
  }else{
    Log("Copy failed!",0);
  }
  utime(to_here.c_str(), &file.times); // overwrite mtime and atime with previous times
  file.path = to_here; // update metadata
  // move File to lower tier list
  std::list<File>::iterator insert_itr = this->lower->files.begin();
  while(insert_itr != this->lower->files.end() && (*insert_itr).priority > file.priority) insert_itr++;
  this->lower->files.insert(insert_itr, file);
  this->files.pop_back();
}

void Tier::tier_up(File &file){
  Log("Tiering up.",2);
  fs::path to_here = this->higher->dir/relative(file.path, this->dir);
  if(is_symlink(to_here)){
    remove(to_here);
  }
  Log("Copying " + file.path.string() + " to " + to_here.string(),2);
  copy_file(file.path, to_here); // move item back to original location
  copy_ownership_and_perms(file.path, to_here);
  if(verify_copy(file.path, to_here)){
    Log("Copy succeeded.",2);
    remove(file.path);
  }else{
    Log("Copy failed!",0);
  }
  utime(to_here.c_str(), &file.times); // overwrite mtime and atime with previous times
  file.path = to_here; // update metadata
  // move File to lower tier list
  std::list<File>::iterator insert_itr = this->higher->files.begin();
  while(insert_itr != this->higher->files.end() && (*insert_itr).priority > file.priority) insert_itr++;
  this->higher->files.insert(insert_itr, file);
  this->files.pop_front();
}

void copy_ownership_and_perms(const fs::path &src, const fs::path &dst){
  struct stat info;
  stat(src.c_str(), &info);
  chown(dst.c_str(), info.st_uid, info.st_gid);
  chmod(dst.c_str(), info.st_mode);
}

bool verify_copy(const fs::path &src, const fs::path &dst){
  int bytes_read;
  char *src_buffer = new char[4096];
  char *dst_buffer = new char[4096];
  
  int srcf = open(src.c_str(),O_RDONLY);
  int dstf = open(dst.c_str(),O_RDONLY);
  
  XXHash64 src_hash(0);
  XXHash64 dst_hash(0);
  
  while((bytes_read = read(srcf,src_buffer,sizeof(char[4096]))) > 0){
    src_hash.add(src_buffer,bytes_read);
  }
  while((bytes_read = read(dstf,dst_buffer,sizeof(char[4096]))) > 0){
    dst_hash.add(dst_buffer,bytes_read);
  }
  
  close(srcf);
  close(dstf);
  delete [] src_buffer;
  delete [] dst_buffer;
  
  uint64_t src_result = src_hash.hash();
  uint64_t dst_result = dst_hash.hash();
  
  std::stringstream ss;
  
  ss << "SRC HASH: 0x" << std::hex << src_result << std::endl;
  ss << "DST HASH: 0x" << std::hex << dst_result << std::endl;
  
  Log(ss.str(),2);
  
  return (src_result == dst_result);
}

struct utimbuf last_times(const fs::path &file){
  struct stat info;
  stat(file.c_str(), &info);
  struct utimbuf times;
  times.actime = info.st_atime;
  times.modtime = info.st_mtime;
  return times;
}

int get_fs_usage(const fs::path &dir, File *file){
  struct statvfs fs_stats;
  if((statvfs(dir.c_str(), &fs_stats) == -1))
    return -1;
  if(file){
    struct stat st;
    stat(file->path.c_str(), &st);
    size_t file_blocks = st.st_size / fs_stats.f_bsize;
    fs_stats.f_bfree -= file_blocks;
  }
  return (int)((fs_stats.f_blocks - fs_stats.f_bfree) * (fsblkcnt_t)100 / fs_stats.f_blocks); 
}

void destroy_tiers(){
  if(highest_tier == lowest_tier){
    delete highest_tier;
    return;
  }
  for(Tier *tptr = highest_tier->lower; tptr != NULL; tptr = tptr->lower)
    delete tptr->higher;
  delete lowest_tier;
}

void dump_tiers(){
  std::cout << "Files from freshest to stalest: " << std::endl;
  
  for(Tier *tptr = highest_tier; tptr != NULL; tptr=tptr->lower){
    std::cout << tptr->id << std::endl;
    for(std::list<File>::iterator itr = tptr->files.begin(); itr != tptr->files.end(); itr++){
      unsigned long tmp = (*itr).priority;
      std::cout << "Prio: " << (*itr).priority << " (";
      for(int i = sizeof(unsigned long) * 8 - 1; i >= 0; i--){
        std::cout << (bool)((*itr).priority & ((unsigned long)0x01 << i));
      }
      std::cout << ") atime: " << (*itr).times.actime << " Location: " << (*itr).path << std::endl;
    }
    std::cout << std::endl;
  }
}
