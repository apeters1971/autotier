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

#include "config.hpp"
#include "alert.hpp"
#include "crawl.hpp"
#include <iostream>
#include <sstream>
#include <fstream>
#include <boost/filesystem.hpp>
#include <regex>

Config config;

void Config::load(const fs::path &config_path){
  config.log_lvl = 1; // default to 1
  std::fstream config_file(config_path.string(), std::ios::in);
  if(!config_file){
    if(!is_directory(config_path.parent_path())) create_directories(config_path.parent_path());
    config_file.open(config_path.string(), std::ios::out);
    this->generate_config(config_file);
    config_file.close();
    config_file.open(config_path.string(), std::ios::in);
  }
  
  Tier *tptr = NULL;
  
  while(config_file){
    std::stringstream line_stream;
    std::string line, key, value;
    
    getline(config_file, line);
    
    // discard comments
    if(line.empty() || line.front() == '#') continue;
    discard_comments(line);
    
    if(line.front() == '['){
      std::string id = line.substr(1,line.find(']')-1);
      if(regex_match(id,std::regex("^\\s*[Gg]lobal\\s*$"))){
        if(this->load_global(config_file, id) == EOF) break;
      }
      if(tptr){
        tptr->lower = new Tier;
        tptr->lower->higher = tptr;
        tptr->lower->lower = NULL;
        tptr = tptr->lower;
      }else{
        tptr = new Tier;
        tptr->higher = tptr->lower = NULL;
        highest_tier = tptr;
      }
      tptr->id = id;
      tptr->expires = DISABLED; // default to disabled until line is read below
    }else if(tptr){
      line_stream.str(line);
      getline(line_stream, key, '=');
      getline(line_stream, value);
      if(key == "DIR"){
        tptr->dir = value;
      }else if(key == "EXPIRES"){
        try{
          tptr->expires = stol(value);
        }catch(std::invalid_argument){
          tptr->expires = ERR;
        }
      }else if(key == "MAX_WATERMARK"){
        try{
          tptr->max_watermark = stoi(value);
        }catch(std::invalid_argument){
          tptr->max_watermark = ERR;
        }
      }else if(key == "MIN_WATERMARK"){
        try{
          tptr->min_watermark = stoi(value);
        }catch(std::invalid_argument){
          tptr->min_watermark = ERR;
        }
      } // else ignore
    }else{
      error(NO_FIRST_TIER);
      exit(1);
    }
  }
  
  lowest_tier = tptr;
    
  if(this->verify()){
    error(LOAD_CONF);
    exit(1);
  }
}

int Config::load_global(std::fstream &config_file, std::string &id){
  while(config_file){
    std::stringstream line_stream;
    std::string line, key, value;
    
    getline(config_file, line);
    
    // discard comments
    if(line.empty() || line.front() == '#') continue;
    discard_comments(line);
    
    if(line.front() == '['){
      id = line.substr(1,line.find(']')-1);
      return 0;
    }
    
    line_stream.str(line);
    getline(line_stream, key, '=');
    getline(line_stream, value);
    
    if(key == "LOG_LEVEL"){
      try{
        this->log_lvl = stoi(value);
      }catch(std::invalid_argument){
        this->log_lvl = ERR;
      }
    } // else if ...
  }
  // if here, EOF reached
  return EOF;
}

void discard_comments(std::string &str){
  std::size_t str_itr;
  if((str_itr = str.find('#')) != std::string::npos){
    str_itr--;
    while(str.at(str_itr) == ' ' || str.at(str_itr) == '\t') str_itr--;
    str = str.substr(0,str_itr + 1);
  }
}

void Config::generate_config(std::fstream &file){
  file <<
  "# autotier config\n"
  "[Global]            # global settings\n"
  "LOG_LEVEL=1         # 0 = none, 1 = normal, 2 = debug\n"
  "\n"
  "[Tier 1]\n"
  "DIR=                # full path to tier storage pool\n"
  "MAX_WATERMARK=      # % usage at which to tier down from tier\n"
  "MIN_WATERMARK=      # % usage at which to tier up into tier\n"
  "# file age is calculated as (current time - file mtime), i.e. the amount\n"
  "# of time that has passed since the file was last modified.\n"
  "[Tier 2]\n"
  "DIR=\n"
  "MAX_WATERMARK=\n"
  "MIN_WATERMARK=\n"
  "# ... (add as many tiers as you like)\n"
  << std::endl;
}

bool Config::verify(){
  bool errors = false;
  if(highest_tier == NULL || lowest_tier == NULL){
    error(NO_TIERS);
    errors = true;
  }else if(highest_tier == lowest_tier){
    error(ONE_TIER);
    errors = true;
  }
  for(Tier *tptr = highest_tier; tptr != NULL; tptr=tptr->lower){
    if(!is_directory(tptr->dir)){
      std::cerr << tptr->id << ": ";
      error(TIER_DNE);
      errors = true;
    }
    if(tptr->expires == ERR){
      std::cerr << tptr->id << ": ";
      error(THRESHOLD_ERR);
      errors = true;
    }
    if(tptr->max_watermark == ERR || tptr->max_watermark > 100 || tptr->max_watermark < 0){
      std::cerr << tptr->id << ": ";
      error(WATERMARK_ERR);
      errors = true;
    }
  }
  return errors;
}

void Config::dump(std::ostream &os) const{
  os << "[Global]" << std::endl;
  os << "LOG_LEVEL=" << this->log_lvl << std::endl;
  os << std::endl;
  for(Tier *tptr = highest_tier; tptr != NULL; tptr=tptr->lower){
    os << "[" << tptr->id << "]" << std::endl;
    os << "DIR=" << tptr->dir.string() << std::endl;
    os << "MAX_WATERMARK=" << tptr->max_watermark << std::endl;
    os << "MIN_WATERMARK=" << tptr->min_watermark << std::endl;
    os << std::endl;
  }
}
