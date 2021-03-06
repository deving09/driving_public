/********************************************************
  Stanford Driving Software
  Copyright (c) 2011 Stanford University
  All rights reserved.

  Redistribution and use in source and binary forms, with
  or without modification, are permitted provided that the
  following conditions are met:

* Redistributions of source code must retain the above
  copyright notice, this list of conditions and the
  following disclaimer.
* Redistributions in binary form must reproduce the above
  copyright notice, this list of conditions and the
  following disclaimer in the documentation and/or other
  materials provided with the distribution.
* The names of the contributors may not be used to endorse
  or promote products derived from this software
  without specific prior written permission.

  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND
  CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED
  WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
  WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
  PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
  INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
  PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
  DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
  OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
  SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
  DAMAGE.
 ********************************************************/

#include "log_and_playback/kittireader.h"
#include <boost/foreach.hpp>
#include <boost/make_shared.hpp>

#include <stdr_lib/exception.h>
#include <blf/vlf.h>
#include <stdr_velodyne/conversion.h>
#include <ladybug_playback/frame_separator.h>

#include <velodyne_pointcloud/rawdata.h>
#include <stdr_velodyne/pointcloud.h>
#include <stdr_velodyne/point_type.h>
#include <stdr_velodyne/config.h>

namespace log_and_playback
{

/*
  Error Acknowledement - Currently within the Kitti Dataset Timestamp are
  stored in increments of 1 Million, this should be 100,000. Until the log files are
  recomputed, the issues will be handled locally in this file. With the ability to
  correct them back into the desired output being commented into this file as well
  */

void KittiApplanixReader::open(const std::string & filename)
{

  file_.open(filename.c_str(), std::ios_base::in);
  stream_.push(file_);
  ok_ = true;
  old_hw_timestamp_ = 0;
}


KittiApplanixReader::~KittiApplanixReader(){
  close();
}

void KittiApplanixReader::close(){
  file_.close();
}

stdr_msgs::ApplanixPose::Ptr KittiApplanixReader::parseApplanix(const std::string & line, double smooth_x,
                                                                double smooth_y, double smooth_z){
  uint64_t epoch_time;
  double data[25];
  char space;
  std::stringstream ss(line);

  //stdr_msgs::ApplanixPose::ConstPtr pose_;

  ss >> epoch_time;

  //Temporary Fix please revert to above line
  //ep_time = static_cast<double>(epoch_time) * 1e-6;
  double ep_time = static_cast<double>(epoch_time) * 1E-7;

  //const double ep_time = static_cast<double>(epoch_time) * 1e-9;//* 1e-9;
  for(int i=0; i<25; i++){
    ss >> data[i];
  }

  stdr_msgs::ApplanixPose::Ptr pose( new stdr_msgs::ApplanixPose );
  pose->latitude = data[0];
  pose->longitude = data[1];
  pose->altitude = data[2];
  pose->roll = data[3];
  pose->pitch = data[4];
  pose->yaw = data[5];
  pose->vel_north = (float)(data[6]);
  pose->vel_east = (float)(data[7]);
  pose->vel_up = (float) (data[10]);
  pose->rate_roll = data[17];
  pose->rate_pitch = data[18];
  pose->rate_yaw = data[19];
  pose->accel_x = data[11];
  pose->accel_y = data[12];
  pose->accel_z = data[13];
  pose->wander =0;
  pose->id = 0;
  pose->speed = (float)(sqrt(pose->vel_north * pose->vel_north
                             + pose->vel_east * pose->vel_east));
  pose->track = (float)(atan2(pose->vel_north, pose->vel_east));

  if (old_hw_timestamp_ !=0){
    double dt = ep_time - old_hw_timestamp_;
    pose->smooth_x = smooth_x + pose->vel_east * dt;
    pose->smooth_y = smooth_y + pose->vel_north * dt;
    pose->smooth_z = smooth_z + pose->vel_up * dt;

  }else{
    pose->smooth_x = 0;
    pose->smooth_y = 0;
    pose->smooth_z = 0;
  }
  old_hw_timestamp_ = ep_time;
  pose->hardware_timestamp = ep_time;
  time_ = ros::Time(ep_time);
  pose->header.stamp = time_;
  return pose;
}

bool KittiApplanixReader::next(){
  double smooth_x, smooth_y, smooth_z;
  smooth_x = smooth_y = smooth_z = 0;
  if(pose_){
      smooth_x = pose_->smooth_x;
      smooth_y = pose_->smooth_y;
      smooth_z = pose_->smooth_z;
  }

  pose_.reset();

  while( true )
  {
    if( ok_ && std::getline(stream_, line_) ) {
      pose_ = parseApplanix(line_, smooth_x, smooth_y, smooth_z);
      if( pose_ ) {
        time_ = pose_->header.stamp;
        return true;
      }
    }
    else {
      ok_ = false;
      return ok_;
    }
  }

  return ok_;
  return false;
}

stdr_msgs::ApplanixPose::ConstPtr
KittiApplanixReader::instantiateApplanixPose() const
{
  return pose_;
}



KittiVeloReader::~KittiVeloReader()
{
  close();
}

KittiVeloReader::KittiVeloReader(){
  config_ = stdr_velodyne::Configuration::getStaticConfigurationInstance();
  ok_ = false;
  spin_ = boost::make_shared<stdr_velodyne::PointCloud>();
}

void KittiVeloReader::open(const std::string & filename)
{
  vfile_.open(filename.c_str());
  ok_ = true;
  config_ = stdr_velodyne::Configuration::getStaticConfigurationInstance();
  ROS_ASSERT(config_);
}

void KittiVeloReader::close()
{
  vfile_.close();
  ok_ = false;
}

bool KittiVeloReader::next()
{

  ROS_ASSERT(config_);

  if( !vfile_ && vfile_.good())
    return false;

  unsigned int num_points;
  uint64_t t_start, t_end;

  try {
    vfile_.read((char *)(&num_points), sizeof(num_points));
    vfile_.read((char *)(&t_start), sizeof(t_start));
    vfile_.read((char *)(&t_end), sizeof(t_end));

    if(!vfile_.good()){
      ok_ = false;
      return ok_;
    }

    //Temporary fix. Delete two line below once log files are fixed
    t_start = uint64_t(t_start * 1e-1);
    t_end = uint64_t(t_end * 1e-1);

    spin_.reset(new stdr_velodyne::PointCloud);
    spin_->reserve(num_points);

    spin_->header.frame_id = "velodyne";
    spin_->header.seq = 14;

    spin_->header.stamp = t_start;

    // Recent Additions
    time_ = pcl_conversions::fromPCL(spin_->header).stamp;
    stdr_velodyne::PointType pt;

    float x,y,z;
    float intensity;
    float distance;
    float h_angle, v_angle;
    double timestamp;
    uint8_t beam_id, beam_nb;
    uint16_t encoder;
    uint32_t rgb;

    for( int i =0; i< num_points; i++){

      // load point info from .kit file
      vfile_.read((char *)(&x), sizeof(x));
      vfile_.read((char *)(&y), sizeof(y));
      vfile_.read((char *)(&z), sizeof(z));
      vfile_.read((char *)(&intensity), sizeof(intensity));
      vfile_.read((char *)(&h_angle), sizeof(h_angle));
      vfile_.read((char *)(&beam_id), sizeof(beam_id));
      vfile_.read((char *)(&distance), sizeof(distance));

      // load configuration data
      ROS_ASSERT(config_);

      const stdr_velodyne::RingConfig & rcfg = config_->getRingConfig(beam_id);
      v_angle = rcfg.vert_angle_.getRads();
      beam_nb = config_->getBeamNumber(beam_id);
      encoder = (uint16_t)(h_angle* 100);

      // update point data
      pt.x = x;
      pt.y = y;
      pt.z = z;
      pt.intensity = intensity * 255;
      pt.h_angle = h_angle;
      pt.encoder = encoder;
      pt.v_angle = v_angle;
      pt.beam_id = beam_id -1 ;
      pt.beam_nb = beam_id - 1; //beam_nb;
     // pt.timestamp = static_cast<double>(t_start) * 1E-6;
      pt.beam_nb = beam_id - 1;//beam_nb;
      pt.timestamp = time_.toSec();
      pt.distance = distance;
      // add to pointcloud
      spin_->push_back(pt);
    }

  }catch (stdr::ex::IOError& e) {
    ok_ = false;
    return ok_;
  }

  ok_ = true;
  return ok_;
}

}
