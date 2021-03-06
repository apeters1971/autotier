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

void TierEngine::begin(){
  Log("autotier started.\n",1);
  launch_crawlers();
  sort();
  simulate_tier();
  move_files();
  Log("Tiering complete.\n",1);
}

void TierEngine::launch_crawlers(){
  Log("Gathering files.",2);
  // get ordered list of files in each tier
  for(std::vector<Tier>::iterator t = tiers.begin(); t != tiers.end(); ++t){
    crawl(t->dir, &(*t));
  }
}

void TierEngine::sort(){
  Log("Sorting files.",2);
  files.sort(
    [](const File &a, const File &b){
      return (a.priority == b.priority)? a.times.actime > b.times.actime : a.priority > b.priority;
    }
  );
}

void TierEngine::crawl(fs::path dir, Tier *tptr){
  for(fs::directory_iterator itr{dir}; itr != fs::directory_iterator{}; *itr++){
    if(is_directory(*itr)){
      crawl(*itr, tptr);
    }else if(!is_symlink(*itr) &&
    !regex_match((*itr).path().filename().string(), std::regex("(^\\..*(\\.swp)$|^(\\.~lock\\.).*#$|^(~\\$))"))){
      files.emplace_back(*itr, tptr);
    }
  }
}

void TierEngine::simulate_tier(){
  Log("Finding files' tiers.",2);
  long tier_use = 0;
  std::list<File>::iterator fptr = files.begin();
  std::vector<Tier>::iterator tptr = tiers.begin();
  tptr->watermark_bytes = tptr->set_capacity();
  while(fptr != files.end()){
    if(tier_use + fptr->size >= tptr->watermark_bytes){
      tier_use = 0;
      if(++tptr == tiers.end()) break;
      tptr->watermark_bytes = tptr->set_capacity();
    }
    tier_use += fptr->size;
    /*
     * TODO: only place file in incoming queue if destination tier != current tier
     */
    tptr->incoming_files.emplace_back(&(*fptr));
    fptr++;
  }
}

void TierEngine::move_files(){
  /*
   * Currently, this starts at the lowest tier, assuming it has the most free space, and
   * moves all incoming files from their current tiers before moving on to the next lowest
   * tier. There should be a better way to shuffle all the files around to avoid over-filling
   * a tier by doing them one at a time.
   */
  Log("Moving files.",2);
  for(std::vector<Tier>::reverse_iterator titr = tiers.rbegin(); titr != tiers.rend(); titr++){
    for(File * fptr : titr->incoming_files){
      fptr->new_path = titr->dir/relative(fptr->old_path, fptr->old_tier->dir);
      fptr->symlink_path = tiers.front().dir/relative(fptr->old_path, fptr->old_tier->dir);
      /*
       * TODO: handle cases where file already exists at destination (should not happen but could)
       */
      if(fptr->new_path != fptr->symlink_path){
        fptr->move();
        if(is_symlink(fptr->symlink_path)) remove(fptr->symlink_path);
        create_symlink(fptr->new_path, fptr->symlink_path);
      }else{ // moving to top tier
        if(is_symlink(fptr->new_path)) remove(fptr->new_path);
        fptr->move();
      }
    }
  }
}

void File::move(){
  if(old_path == new_path) return;
  if(!is_directory(new_path.parent_path()))
    create_directories(new_path.parent_path());
  Log("Copying " + old_path.string() + " to " + new_path.string(),2);
  copy_file(old_path, new_path); // move item to slow tier
  copy_ownership_and_perms(old_path, new_path);
  if(verify_copy(old_path, new_path)){
    Log("Copy succeeded.",2);
    remove(old_path);
  }else{
    Log("Copy failed!",0);
    /*
     * TODO: put in place protocol for what to do when this happens
     */
  }
  utime(new_path.c_str(), &times); // overwrite mtime and atime with previous times
}

void copy_ownership_and_perms(const fs::path &src, const fs::path &dst){
  struct stat info;
  stat(src.c_str(), &info);
  chown(dst.c_str(), info.st_uid, info.st_gid);
  chmod(dst.c_str(), info.st_mode);
}

bool verify_copy(const fs::path &src, const fs::path &dst){
  /*
   * TODO: more efficient error checking than this? Also make
   * optional in global configuration?
   */
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

long Tier::set_capacity(){
  /*
   * Returns maximum number of bytes to
   * place in a tier (Total size * watermark%)
   */
  struct statvfs fs_stats;
  if((statvfs(dir.c_str(), &fs_stats) == -1))
    return -1;
  return (fs_stats.f_blocks * fs_stats.f_bsize * watermark) / 100;
}
