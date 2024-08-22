// Copyright (c) 2020
// Commonwealth Scientific and Industrial Research Organisation (CSIRO)
// ABN 41 687 119 230
//
// Author: Thomas Lowe
#include "raydecimation.h"
#include <iostream>
#include <limits>
#include <map>
#include "raycloudwriter.h"

namespace ray
{
bool decimateSpatial(const std::string &file_stub, double vox_width)
{
  ray::CloudWriter writer;
  if (!writer.begin(file_stub + "_decimated.ply"))
    return false;

  // By maintaining these buffers below, we avoid almost all memory fragmentation
  ray::Cloud chunk;
  std::vector<int64_t> subsample;
  std::set<Eigen::Vector3i, ray::Vector3iLess> voxel_set;

  auto decimate = [&](std::vector<Eigen::Vector3d> &starts, std::vector<Eigen::Vector3d> &ends,
                      std::vector<double> &times, std::vector<ray::RGBA> &colours) 
  {
    double width = 0.01 * vox_width;
    subsample.clear();
    voxelSubsample(ends, width, subsample, voxel_set);
    chunk.resize(subsample.size());
    for (int64_t i = 0; i < (int64_t)subsample.size(); i++)
    {
      int64_t id = subsample[i];
      chunk.starts[i] = starts[id];
      chunk.ends[i] = ends[id];
      chunk.colours[i] = colours[id];
      chunk.times[i] = times[id];
    }
    writer.writeChunk(chunk);
  };

  if (!ray::Cloud::read(file_stub + ".ply", decimate))
    return false;
  writer.end();
  return true;
}

bool decimateTemporal(const std::string &file_stub, int num_rays)
{
  ray::CloudWriter writer;
  if (!writer.begin(file_stub + "_decimated.ply"))
    return false;

  // By maintaining these buffers below, we avoid almost all memory fragmentation
  ray::Cloud chunk;
  auto decimate = [&](std::vector<Eigen::Vector3d> &starts, std::vector<Eigen::Vector3d> &ends,
                      std::vector<double> &times, std::vector<ray::RGBA> &colours) 
  {
    size_t decimation = (size_t)num_rays;
    size_t count = (ends.size() + decimation - 1) / decimation;
    chunk.resize(count);
    for (size_t i = 0, c = 0; i < ends.size(); i += decimation, c++)
    {
      chunk.starts[c] = starts[i];
      chunk.ends[c] = ends[i];
      chunk.times[c] = times[i];
      chunk.colours[c] = colours[i];
    }
    writer.writeChunk(chunk);
  };

  if (!ray::Cloud::read(file_stub + ".ply", decimate))
    return false;
  writer.end();
  return true;
}

bool decimateSpatioTemporal(const std::string &file_stub, double vox_width, int num_rays)
{
  ray::CloudWriter writer;
  if (!writer.begin(file_stub + "_decimated.ply"))
    return false;

  // By maintaining these buffers below, we avoid almost all memory fragmentation
  ray::Cloud chunk;
  std::map<Eigen::Vector3i, Eigen::Vector2i, ray::Vector3iLess> voxel_map;
  std::vector<Eigen::Vector3i> samples;

  auto decimate = [&](std::vector<Eigen::Vector3d> &, std::vector<Eigen::Vector3d> &ends,
                      std::vector<double> &, std::vector<ray::RGBA> &) 
  {
    double voxel_width = 0.01 * vox_width;
    // firstly we store a count per cell
    for (size_t i = 0; i<ends.size(); i++)
    {
      Eigen::Vector3d coords = ends[i] / voxel_width;
      Eigen::Vector3i coordsi = Eigen::Vector3d(std::floor(coords[0]), std::floor(coords[1]), std::floor(coords[2])).cast<int>();
      auto found = voxel_map.find(coordsi);
      if (found == voxel_map.end())
      {
        voxel_map.insert(std::pair<Eigen::Vector3i, Eigen::Vector2i>(coordsi, Eigen::Vector2i(1,0)));
        samples.push_back(coordsi);
      } 
      else
      {
        found->second[0]++;
      }      
    }
    writer.writeChunk(chunk);
  };

  if (!ray::Cloud::read(file_stub + ".ply", decimate))
    return false;

  double voxel_width = 0.01 * vox_width;
  for (auto &pos: samples)
  {
    int max_num = 0;
    for (int x = pos[0]-1; x<=pos[0]+1; x++)
    {
      for (int y = pos[1]-1; y<=pos[1]+1; y++)
      {
        for (int z = pos[2]-1; z<=pos[2]+1; z++)
        {
          auto found = voxel_map.find(Eigen::Vector3i(x,y,z));
          if (found != voxel_map.end())
            max_num = std::max(max_num, found->second[0]);  // TODO: max of neighbours, or max 2x2 of neighbours?
        }
      }
    }
    voxel_map.find(Eigen::Vector3i(pos[0],pos[1],pos[2]))->second[1] = max_num;
  }

  auto finalise = [&](std::vector<Eigen::Vector3d> &starts, std::vector<Eigen::Vector3d> &ends,
                      std::vector<double> &times, std::vector<ray::RGBA> &colours) 
  {
    chunk.resize(0);
    for (size_t i = 0; i<ends.size(); i++)
    {
      Eigen::Vector3d coords = ends[i] / voxel_width;
      Eigen::Vector3i coordsi = Eigen::Vector3d(std::floor(coords[0]), std::floor(coords[1]), std::floor(coords[2])).cast<int>();
      auto found = voxel_map.find(coordsi);
      if (found != voxel_map.end())
      {
        int num = (found->second)[1];
        double segmentation = std::max(1.0, (double)num / (double)num_rays);
        int &ends_left = (found->second)[0]; 
        if (std::fmod((double)ends_left+1.0, segmentation) <= std::fmod((double)ends_left, segmentation))
        {
          chunk.starts.push_back(starts[i]);
          chunk.ends.push_back(ends[i]);
          chunk.colours.push_back(colours[i]);
          chunk.times.push_back(times[i]);
        }
        ends_left--;
      }
    }
    writer.writeChunk(chunk);
  };
  if (!ray::Cloud::read(file_stub + ".ply", finalise))
    return false;   
  writer.end();
  return true;
}

int sign(double x)
{
  return (x > 0.0) - (x < 0.0);
}

bool decimateRaysSpatial(const std::string &file_stub, double vox_width)
{
  ray::CloudWriter writer;
  if (!writer.begin(file_stub + "_decimated.ply"))
    return false;

  // By maintaining these buffers below, we avoid almost all memory fragmentation
  ray::Cloud chunk;
  std::vector<int64_t> subsample;
  std::set<Eigen::Vector3i, ray::Vector3iLess> voxel_set;

  auto decimate = [&](std::vector<Eigen::Vector3d> &starts, std::vector<Eigen::Vector3d> &ends,
                      std::vector<double> &times, std::vector<ray::RGBA> &colours) 
  {
    double width = 0.01 * vox_width;
    subsample.clear();
    for (int i = 0; i < (int)ends.size(); i++)
    {
      // #define END_FIRST // by traversing from end to start we match the existence of the ray more to the free space near the end point. In building1 test start first found more ray locations
      #if defined END_FIRST
      Eigen::Vector3d dir = starts[i] - ends[i];
      const Eigen::Vector3d source = ends[i] / width;
      const Eigen::Vector3d target = starts[i] / width;
      #else
      Eigen::Vector3d dir = ends[i] - starts[i];
      const Eigen::Vector3d source = starts[i] / width;
      const Eigen::Vector3d target = ends[i] / width;
      #endif

      #define OLD_METHOD
      #if defined OLD_METHOD
      const double length = dir.norm();
      dir /= length; 
      for (int a = 0; a<3; a++)
      {
        if (dir[a] == 0.0)
        {
          dir[a] = 1e-10; // prevent division by 0
        }
      }
      const double eps = 1e-9;  // to stay away from edge cases
      const double maxDist = (target - source).norm();

      // cached values to speed up the loop below
      Eigen::Vector3i adds;
      Eigen::Vector3d offsets;
      for (int k = 0; k < 3; ++k)
      {
        if (dir[k] > 0.0)
        {
          adds[k] = 1;
          offsets[k] = 0.5;
        }
        else
        {
          adds[k] = -1;
          offsets[k] = -0.5;
        }
      }

      Eigen::Vector3d p = source;  // our moving variable as we walk over the grid
      Eigen::Vector3i inds((int)std::floor(p[0]), (int)std::floor(p[1]), (int)std::floor(p[2]));
      if (voxel_set.insert(inds).second)
      {
        subsample.push_back(i);
        continue;
      }
      double depth = 0;
      // for every ray, walk over its length and if there is a free space then add it
      do
      {
        double ls[3] = { (std::round(p[0] + offsets[0]) - p[0]) / dir[0], (std::round(p[1] + offsets[1]) - p[1]) / dir[1],
                         (std::round(p[2] + offsets[2]) - p[2]) / dir[2] };
        int axis = (ls[0] < ls[1] && ls[0] < ls[2]) ? 0 : (ls[1] < ls[2] ? 1 : 2);
        inds[axis] += adds[axis];
        depth += ls[axis] + eps;
        p = source + dir * depth;
        
        if (voxel_set.insert(inds).second)
        {
          subsample.push_back(i);
          break; // only adding to one cell
        }
      } while (depth <= maxDist);
      #else
        Eigen::Vector3i p = Eigen::Vector3d(std::floor(source[0]), std::floor(source[1]), std::floor(source[2])).cast<int>();
        Eigen::Vector3i end = Eigen::Vector3d(std::floor(target[0]), std::floor(target[1]), std::floor(target[2])).cast<int>();
        
        if (voxel_set.insert(p).second)
        {
          std::cout << "missed p: " << p.transpose() << " between " << source.transpose() << " and " << target.transpose() << std::endl;
          subsample.push_back(i);
          continue; // only adding to one cell
        }
        
        Eigen::Vector3i step(sign(target[0] - source[0]), sign(target[1] - source[1]), sign(target[2] - source[2]));
        Eigen::Vector3d tmax, tdelta;
        for (int j = 0; j<3; j++)
        {
          step[j] = sign(target[j] - source[j]);
          double to = std::abs(source[j] - p[j] - (double)std::max(0, step[j]));        
          double dir = std::max(std::numeric_limits<double>::min(), std::abs(source[j] - target[j]));
          tmax[j] = to / dir;
          tdelta[j] = 1.0 / dir;
        }
        
        while (p != end) 
        {
          int ax = tmax[0] < tmax[1] && tmax[0] < tmax[2] ? 0 : (tmax[1] < tmax[2] ? 1 : 2);
          p[ax] += step[ax];
          tmax[ax] += tdelta[ax];
     /*     if (tmax[0] < tmax[1]) 
          {
            if (tmax[0] < tmax[2]) 
            {
              p[0] += step[0];
              tmax[0] += tdelta[0];
            } 
            else 
            {
              p[2] += step[2];
              tmax[2] += tdelta[2];
            }
          } 
          else 
          {
            if (tmax[1] < tmax[2]) 
            {
              p[1] += step[1];
              tmax[1] += tdelta[1];
            } 
            else 
            {
              p[2] += step[2];
              tmax[2] += tdelta[2];
            }
          }*/
          if (voxel_set.insert(p).second)
          {
            std::cout << "missed p: " << p.transpose() << " between " << source.transpose() << " and " << target.transpose() << std::endl;
            subsample.push_back(i);
            break; // only adding to one cell
          }          
        }      
       #endif
    }
    chunk.resize(subsample.size());
    for (int64_t i = 0; i < (int64_t)subsample.size(); i++)
    {
      int64_t id = subsample[i];
      chunk.starts[i] = starts[id];
      chunk.ends[i] = ends[id];
      chunk.colours[i] = colours[id];
      chunk.times[i] = times[id];
    }
    writer.writeChunk(chunk);
  };

  if (!ray::Cloud::read(file_stub + ".ply", decimate))
    return false;
  writer.end();
  return true;
}

bool decimateAngular(const std::string &file_stub, double radius_per_length)
{
  ray::CloudWriter writer;
  if (!writer.begin(file_stub + "_decimated.ply"))
    return false;

  ray::Cloud chunk;

  int min_index = -20; // about a millimetre
  int max_index = 50;
  std::vector<std::set<Eigen::Vector3i, ray::Vector3iLess>> voxel_sets(max_index + 1 - min_index);
  std::vector<std::set<Eigen::Vector3i, ray::Vector3iLess>> visiteds(max_index + 1 - min_index);
  std::vector<int> candidate_indices;  
  const double root2 = std::sqrt(2.0);
  const double logroot2 = std::log(root2);
  std::vector<double> voxel_widths(voxel_sets.size()); 
  for (int i = 0; i<(int)voxel_widths.size(); i++)
  {
    voxel_widths[i] = std::pow(root2, (double)(i+min_index));
  }
  int index = -1;

  auto decimate = [&](std::vector<Eigen::Vector3d> &starts, std::vector<Eigen::Vector3d> &ends,
                      std::vector<double> &, std::vector<ray::RGBA> &) 
  {
    for (size_t i = 0; i<ends.size(); i++)
    {
      index++;
      double radius = (starts[i] - ends[i]).norm() * 0.01*radius_per_length;
      int map_index = std::max(min_index, std::min((int)std::round(std::log(2.0*radius)/logroot2), max_index));
      Eigen::Vector3d coords = ends[i] / voxel_widths[map_index - min_index];
      Eigen::Vector3i coordsi = Eigen::Vector3d(std::floor(coords[0]), std::floor(coords[1]), std::floor(coords[2])).cast<int>();
      int ind = map_index - min_index;
      if (visiteds[ind].find(coordsi) != visiteds[ind].end()) // this level map has already been visited by a child (smaller ray length)
        continue;

      if (voxel_sets[ind].insert(coordsi).second)
      {
        candidate_indices.push_back(index);
        // now insert visiteds to suppress longer rays
        Eigen::Vector3i pos = coordsi;
        double scale = root2;
        pos = Eigen::Vector3d(std::floor((double)coordsi[0]/scale), std::floor((double)coordsi[1]/scale), std::floor((double)coordsi[2]/scale)).cast<int>();
        ind++;
        while (ind < (int)visiteds.size() && visiteds[ind].insert(pos).second)
        {
          ind++;
          scale *= root2;
          pos = Eigen::Vector3d(std::floor((double)coordsi[0]/scale), std::floor((double)coordsi[1]/scale), std::floor((double)coordsi[2]/scale)).cast<int>();
        }         
      }
    }
    writer.writeChunk(chunk);
  };

  if (!ray::Cloud::read(file_stub + ".ply", decimate))
    return false;

  std::cout << "finalising" << std::endl;
  for (auto &map: voxel_sets)
    map.clear(); // redo
  index = -1;
  int head = 0;
  // the finalise step uses the visiteds data to decide whether to include each ray
  auto finalise = [&](std::vector<Eigen::Vector3d> &starts, std::vector<Eigen::Vector3d> &ends,
                      std::vector<double> &times, std::vector<ray::RGBA> &colours) 
  {
    chunk.resize(0);
    for (size_t i = 0; i<ends.size(); i++)
    {
      index++;
      if (index != candidate_indices[head])
        continue;
      head++;
      double radius = (starts[i] - ends[i]).norm() * 0.01*radius_per_length;
      int map_index = std::max(min_index, std::min((int)std::round(std::log(2.0*radius)/logroot2), max_index));
      Eigen::Vector3d coords = ends[i] / voxel_widths[map_index - min_index];
      Eigen::Vector3i coordsi = Eigen::Vector3d(std::floor(coords[0]), std::floor(coords[1]), std::floor(coords[2])).cast<int>();
      int ind = map_index - min_index;
      if (visiteds[ind].find(coordsi) == visiteds[ind].end()) 
      {
        chunk.starts.push_back(starts[i]);
        chunk.ends.push_back(ends[i]);
        chunk.colours.push_back(colours[i]);
        chunk.times.push_back(times[i]);
      }
    }
    writer.writeChunk(chunk);
  };
  if (!ray::Cloud::read(file_stub + ".ply", finalise))
    return false;
  writer.end();
  return true;
}
}  // namespace ray
