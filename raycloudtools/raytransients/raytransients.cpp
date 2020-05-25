// Copyright (c) 2020
// Commonwealth Scientific and Industrial Research Organisation (CSIRO)
// ABN 41 687 119 230
//
// Author: Thomas Lowe
#include "raylib/raycloud.h"
#include "raylib/raydebugdraw.h"
#include "raylib/raymesh.h"
#include "raylib/rayply.h"
#include "raylib/rayprogress.h"

#define NEW_FILTER 1
#if NEW_FILTER
#include "raylib/raytransientfilter.h"
#else  // NEW_FILTER
#include "raylib/raymerger.h"
#endif // NEW_FILTER

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <thread>

void usage(int exit_code = 0)
{
  std::cout << "Splits a raycloud into the transient rays and the fixed part" << std::endl;
  std::cout << "usage:" << std::endl;
  std::cout << "raytransients min raycloud 20 rays - splits out positive transients (objects that have since moved)."
       << std::endl;
  std::cout << "                                     20 is number of pass through rays to classify as transient." << std::endl;
  std::cout << "              max    - finds negative transients, such as a hallway exposed when a door opens." << std::endl;
  std::cout << "              oldest - keeps the oldest geometry when there is a difference over time." << std::endl;
  std::cout << "              newest - uses the newest geometry when there is a difference over time." << std::endl;
  std::cout << " --colour     - also colours the clouds, to help tweak numRays. red: opacity, green: pass throughs, blue: "
          "planarity." << std::endl;
  exit(exit_code);
}

void runProrgess(const ray::Progress &progress, std::atomic_bool &quit)
{
  ray::Progress last;
  ray::Progress current;
  progress.read(&last);

  const auto show_progress = [] (ray::Progress &p, bool finalise) //
  {
    if (finalise)
    {
      // Finalise progress == target.
      if (p.target())
      {
        p.setProgress(p.target());
      }
    }

    if (p.phase().length() || p.target() || p.progress())
    {
      std::cout << "\r                                    \r";
      std::cout << p.phase() << ' ' << p.progress();
      if (size_t target = p.target())
      {
        std::cout << " / " << target;
      }

      if (finalise)
      {
        std::cout << std::endl;
      }
      else
      {
        std::cout << std::flush;
      }
    }
  };

  while (!quit)
  {
    progress.read(&current);
    if (current.phase() != last.phase())
    {
      // Ensure we finalise the display.
      show_progress(last, true);
    }

    if ( current.progress() != last.progress() || current.target() != last.target())
    {
      show_progress(current, false);
      current.read(&last);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }

  // Past update.
  progress.read(&current);
  // Do not finalise in case we didn't go full progress.
  show_progress(last, false);
  std::cout << std::endl;
}

int main(int argc, char *argv[])
{
  ray::DebugDraw::init(argc, argv, "raytransients");
  if (argc != 5 && argc != 6)
    usage();

  bool colour = false;
  if (argc == 6)
  {
    if (std::string(argv[5]) != "--colour" && std::string(argv[5]) != "-c")
      usage();
    colour = true;
  }
  double num_rays = std::stod(argv[3]);
  std::string merge_type = argv[1];
  if (merge_type != "min" && merge_type != "max" && merge_type != "oldest" && merge_type != "newest")
    usage();
  std::string file = argv[2];
  ray::Cloud cloud;
  cloud.load(file);

#if NEW_FILTER
  ray::TransientFilterConfig config;
  // Note: we actually get better multi-threaded performace with smaller voxels
  config.voxel_size = 0.1;
  config.num_rays_filter_threshold = num_rays;
  // config.strategy = ray::TransientFilterStrategy::EllipseGrid;
  config.strategy = ray::TransientFilterStrategy::RayGrid;
  config.merge_type = ray::MergeType::Mininum;
  config.colour_cloud = colour;

  if (merge_type == "oldest")
  {
    config.merge_type = ray::MergeType::Oldest;
  }
  if (merge_type == "newest")
  {
    config.merge_type = ray::MergeType::Newest;
  }
  if (merge_type == "min")
  {
    config.merge_type = ray::MergeType::Mininum;
  }
  if (merge_type == "max")
  {
    config.merge_type = ray::MergeType::Maximum;
  }

  ray::TransientFilter filter(config);
  ray::Progress progress;
  std::atomic_bool quit_progress(false);
  std::thread progress_thread([&progress, &quit_progress]() { runProrgess(progress, quit_progress); });

  filter.filter(cloud, &progress);

  quit_progress = true;
  progress_thread.join();

  const ray::Cloud &transient = filter.transientCloud();
  const ray::Cloud &fixed = filter.fixedCloud();

#else   // NEW_FILTER
  ray::Merger merger;
  merger.mergeSingleCloud(cloud, merge_type, num_rays, colour);

  const ray::Cloud &transient = merger.differenceCloud();
  const ray::Cloud &fixed = merger.fixedCloud();
#endif  // NEW_FILTER

  std::string file_stub = file;
  if (file.substr(file.length() - 4) == ".ply")
    file_stub = file.substr(0, file.length() - 4);

  transient.save(file_stub + "_transient.ply");
  fixed.save(file_stub + "_fixed.ply");
  return 0;
}
