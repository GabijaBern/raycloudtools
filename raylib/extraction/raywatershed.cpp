// Copyright (c) 2020
// Commonwealth Scientific and Industrial Research Organisation (CSIRO)
// ABN 41 687 119 230
//
// Author: Thomas Lowe
#include "rayforest.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <iostream>
#include <queue>
#include "../rayply.h"

namespace ray
{

void Forest::renderWatershed(const std::string &cloud_name_stub, std::vector<TreeNode> &trees, std::vector<int> &indices)
{
  if (!verbose)
    return;
  std::vector<Eigen::Vector3d> cloud_points;
  std::vector<double> times;
  std::vector<RGBA> colours;
  RGBA colour;
  colour.alpha = 255;

  for (int x = 0; x < indexfield_.rows(); x++)
  {
    for (int y = 0; y < indexfield_.cols(); y++)
    {
      int ind = indexfield_(x, y);
      if (ind == -1)
        continue;
      while (trees[ind].attaches_to != -1)
      {
        if (std::find(indices.begin(), indices.end(), ind) != indices.end())
          break;
        ind = trees[ind].attaches_to;
      }
      if (std::find(indices.begin(), indices.end(), ind) == indices.end())
        continue;
      srand(1 + ind);
      colour.red = (uint8_t)(rand()%256);
      colour.green = (uint8_t)(rand()%256);
      colour.blue = (uint8_t)(rand()%256);
      Eigen::Vector3d pos = min_bounds_ + voxel_width_*Eigen::Vector3d(0.5 + (double)x, 0.5 + (double)y, 0);
      pos[2] = heightfield_(x, y);
      cloud_points.push_back(pos);
      times.push_back(0.0);
      colours.push_back(colour);
    }
  }

  for (auto &ind: indices)
  {
    Eigen::Vector3d tip;
    srand(1 + ind);
    colour.red = (uint8_t)(rand()%256);
    colour.green = (uint8_t)(rand()%256);
    colour.blue = (uint8_t)(rand()%256);
    double z_max = 2.0;
    if (trees[ind].trunk_id >= 0)
      z_max = 4.0;
    if (findSpace2(trees[ind], tip))
    {
      Eigen::Vector3d base = min_bounds_ + tip;
      base[2] = lowfield_(int(tip[0] / voxel_width_), int(tip[1] / voxel_width_));
      double height_per_radius = 50.0;
      double rad = tip[2] / height_per_radius;

      for (double z = 0.0; z<z_max; z+=0.3)
      {
        for (double ang = 0.0; ang<2.0*kPi; ang += 0.3)
        {
          cloud_points.push_back(base + Eigen::Vector3d(rad*std::sin(ang), rad*std::cos(ang), z));
          times.push_back(0.0);
          colours.push_back(colour);
        }
      }
    }
  }

  // now add the space field:
  for (int i = 0; i<spacefield_.rows(); i++)
  {
    for (int j = 0; j<spacefield_.cols(); j++)
    {
      if (spacefield_(i,j) < 1.0)
      {
        double height = lowfield_(i, j) + 0.2;
        double x = min_bounds_[0] + (double)i*voxel_width_;
        double y = min_bounds_[1] + (double)j*voxel_width_;
        cloud_points.push_back(Eigen::Vector3d(x, y, height));
        times.push_back(0.0);
        colour.red = colour.green = colour.blue = (uint8_t)(255.0*spacefield_(i,j));
        colours.push_back(colour);
      }
    }
  }  

  writePlyPointCloud(cloud_name_stub + "_watershed.ply", cloud_points, times, colours);  
}

double Forest::searchTrees(const std::vector<TreeNode> &trees, int ind, double length_per_radius, std::vector<int> &indices)
{
  // length estimated from both the pixel coverage and the paraboloid curvature
//  double length = length_per_radius * std::sqrt(trees[ind].node.crownRadius() * trees[ind].approx_radius);
//  double base = trees[ind].node.height() - length;
//  double error = abs(base - trees[ind].ground_height);

  double baseA = trees[ind].node.height() - length_per_radius * trees[ind].node.crownRadius();
  double baseB = trees[ind].node.height() - length_per_radius * trees[ind].approx_radius;
  
  // we can justify the below condition working best as:
  // sometimes the pixel area or curvature are just plain bad, so if at least one is good, then this tells us that it is a good place to split.
  // i.e. is it works well with a fat tailed error distribution for each (baseA and baseB)
  double error = std::sqrt(abs(baseA - trees[ind].ground_height) * abs(baseB - trees[ind].ground_height));
  if (trees[ind].children[0] == -1)
  {
    if (trees[ind].validParaboloid(max_tree_canopy_width_to_height_ratio, voxel_width_))
    {
      indices.push_back(ind);
      return error;
    }
    return 1e20;
  }
  std::vector<int> child_indices[2];
  int ind0 = trees[ind].children[0];
  int ind1 = trees[ind].children[1];
  double child_error = searchTrees(trees, ind0, length_per_radius, child_indices[0]);
  if (ind1 != -1)
  {
    child_error = (child_error + searchTrees(trees, ind1, length_per_radius, child_indices[1])) / 2.0; // mean error
  }
  if (error < child_error && trees[ind].validParaboloid(max_tree_canopy_width_to_height_ratio, voxel_width_))
  {
    indices.push_back(ind);
    return error;
  }

  indices.insert(indices.end(), child_indices[0].begin(), child_indices[0].end());
  indices.insert(indices.end(), child_indices[1].begin(), child_indices[1].end());
  return child_error;
}

struct Point 
{ 
  int x, y, index; // if index == -2 then we are merging
  double height;
};
struct PointCmp 
{
  bool operator()(const Point& lhs, const Point& rhs) const 
  { 
    return lhs.height < rhs.height; 
  }
};

void Forest::hierarchicalWatershed(std::vector<TreeNode> &trees, std::set<int> &heads)
{
  // fast array lookup of trunk centres:
  Eigen::ArrayXXi trunkfield = Eigen::ArrayXXi::Constant(indexfield_.rows(), indexfield_.cols(), -1);
  for (int c = 0; c<(int)trunks_.size(); c++) // if there are known trunks, then include them...
  {
    auto &trunk = trunks_[c];
    Eigen::Vector3i pos = ((trunk.first - min_bounds_)/voxel_width_).cast<int>();
    if (pos[0] < 0 || pos[0] >= trunkfield.rows() || pos[1] < 0 || pos[1] >= trunkfield.cols())
    {
      std::cout << "warning: trunk " << c << " location is out of bounds" << std::endl;
      continue;
    }
    trunkfield(pos[0], pos[1]) = c;
  }


  std::priority_queue<Point, std::vector<Point>, PointCmp> basins;
  // 1. find highest points
  for (int x = 0; x < heightfield_.rows(); x++)
  {
    for (int y = 0; y < heightfield_.cols(); y++)
    {
      // Moore neighbourhood
      double height = heightfield_(x, y);
      double max_h = 0.0;
      for (int i = std::max(0, x-1); i<= std::min(x+1, (int)heightfield_.rows()-1); i++)
        for (int j = std::max(0, y-1); j<= std::min(y+1, (int)heightfield_.cols()-1); j++)
          if (!(i==x && j==y))
            max_h = std::max(max_h, heightfield_(i, j));
      if (height > max_h && height > -1e10)
      {
        Point p;
        p.x = x; p.y = y; p.height = height;
        p.index = (int)basins.size();
        basins.push(p);
        heads.insert(p.index);
        indexfield_(x, y) = p.index;     
        trees.push_back(TreeNode(x, y, height, voxel_width_, trunkfield(x, y)));
      }
    }
  }



  std::cout << "initial number of peaks: " << trees.size() << std::endl;
  int cnt = 0;
  // now iterate until basins is empty
  // Below, don't divide by voxel_width, if you want to verify voxel_width independence
  int max_tree_pixel_width = (int)(max_tree_canopy_width_to_height_ratio / (double)voxel_width_); 
  while (!basins.empty())
  {
    Point p = basins.top();
    basins.pop(); // removes it from basins. p still exists
    int x = p.x;
    int y = p.y;

    if (p.index == -2) // a merge request
    {
      int p_head = x;
      while (trees[p_head].attaches_to != -1)
        p_head = trees[p_head].attaches_to;
      int q_head = y;
      while (trees[q_head].attaches_to != -1)
        q_head = trees[q_head].attaches_to;
      bool separate_trunks = trees[p_head].trunk_id >= 0 && trees[q_head].trunk_id >= 0 && trees[p_head].trunk_id != trees[q_head].trunk_id;
      if (p_head != q_head && !separate_trunks) // not already merged and don't have differing trunk ids
      {
        TreeNode &p_tree = trees[p_head];
        TreeNode &q_tree = trees[q_head];
        
        TreeNode node;
        node.peak = p_tree.peak[2] > q_tree.peak[2] ? p_tree.peak : q_tree.peak;
        int x = (int)(node.peak[0]/voxel_width_);
        int y = (int)(node.peak[1]/voxel_width_);   
        x = std::max(0, std::min(x, (int)lowfield_.rows()-1));
        y = std::max(0, std::min(y, (int)lowfield_.cols()-1));
        double tree_height = std::max(0.0, node.peak[2] - lowfield_(x, y)); 

        Eigen::Vector2i mx = ray::maxVector2(p_tree.max_bound, q_tree.max_bound);
        Eigen::Vector2i mn = ray::minVector2(p_tree.min_bound, q_tree.min_bound);
        mx -= mn;
        if (std::max(mx[0], mx[1]) <= max_tree_pixel_width*std::sqrt(tree_height))
        {
          int new_index = (int)trees.size();
          node.min_bound = p_tree.min_bound;
          node.max_bound = p_tree.max_bound;
          node.updateBound(q_tree.min_bound, q_tree.max_bound);
          node.children[0] = p_head;  
          node.children[1] = q_head;
          node.trunk_id = p_tree.trunk_id >= 0 ? p_tree.trunk_id : q_tree.trunk_id;

 //         if (node.validParaboloid(max_tree_canopy_width_to_height_ratio, voxel_width_)) 
          {
            heads.erase(p_head);
            heads.erase(q_head);
            heads.insert(new_index);
            p_tree.attaches_to = new_index;
            q_tree.attaches_to = new_index;
            trees.push_back(node); // danger, this can invalidate the p_tree reference
          }
        }
      }
      continue;
    }    

    int xs[4] = {x-1, x, x, x+1};
    int ys[4] = {y, y+1, y-1, y};
    for (int i = 0; i<4; i++)
    {
      if (xs[i] < 0 || xs[i] >= indexfield_.rows())
        continue;
      if (ys[i] < 0 || ys[i] >= indexfield_.cols())
        continue;
      int p_head = p.index;
      while (trees[p_head].attaches_to != -1)
        p_head = trees[p_head].attaches_to;
        
      int xx = xs[i];
      int yy = ys[i];
      int &ind = indexfield_(xx, yy);

      int q_head = ind;
      if (q_head != -1)
      {
        while (trees[q_head].attaches_to != -1)
          q_head = trees[q_head].attaches_to;
      }

      if (ind != -1 && p_head != q_head) // connecting separate trees, so trigger a future merge event
      {
        TreeNode &p_tree = trees[p_head];
        TreeNode &q_tree = trees[q_head];
        Eigen::Vector2i mx = ray::maxVector2(p_tree.max_bound, q_tree.max_bound);
        Eigen::Vector2i mn = ray::minVector2(p_tree.min_bound, q_tree.min_bound);
        mx -= mn;
        
        Eigen::Vector3d peak = p_tree.peak[2] > q_tree.peak[2] ? p_tree.peak : q_tree.peak;
        int x = (int)(peak[0]/voxel_width_);
        int y = (int)(peak[1]/voxel_width_);   
        x = std::max(0, std::min(x, (int)lowfield_.rows()-1));
        y = std::max(0, std::min(y, (int)lowfield_.cols()-1));
        double tree_height = std::max(0.0, peak[2] - lowfield_(x, y)); 

        bool merge = std::max(mx[0], mx[1]) <= max_tree_pixel_width * std::sqrt(tree_height);
        bool separate_trunks = p_tree.trunk_id >= 0 && q_tree.trunk_id >= 0 && p_tree.trunk_id != q_tree.trunk_id;
        if (merge && !separate_trunks)
        {
          const double flood_merge_scale = 2.0; // 1 merges immediately, infinity never merges
          // add a merge task:
          Eigen::Vector2d mid = Eigen::Vector2d(xx, yy) * voxel_width_;
          Eigen::Vector2d ptree(p_tree.peak[0], p_tree.peak[1]);
          Eigen::Vector2d qtree(q_tree.peak[0], q_tree.peak[1]);
          double blend = (mid - ptree).dot(qtree-ptree) / (qtree-ptree).squaredNorm();
          double flood_base = p_tree.peak[2]*(1.0-blend) + q_tree.peak[2]*blend;
          double low_flood_height = flood_base - p.height;

          Point q;
          q.x = p_head; q.y = q_head; 
          q.index = -2;
          q.height = flood_base - low_flood_height * flood_merge_scale;
          basins.push(q);
        }
      }
      if (ind == -1 && heightfield_(xx, yy) > -1e10) // adding a single pixel to a tree
      {
        Point q;
        q.x = xx; q.y = yy; 
        q.height = heightfield_(xx, yy);
        cnt++;
 
        int trunkid = trunkfield(xx, yy);
        if (trunkid >= 0)
        {
          if (trees[p_head].trunk_id == -1)
          {
            trees[p_head].trunk_id = trunkid;
          }
          else // a second trunk on a downward slope, we'll have to make a whole new treenodde
          {
            p_head = (int)trees.size(); // this new pixel will point to the new tree node here
            trees.push_back(TreeNode(xx, yy, q.height, voxel_width_, trunkid));
          }
        }    
        q.index = p_head;
        ind = p_head;
        basins.push(q);
        trees[p_head].updateBound(Eigen::Vector2i(xx, yy), Eigen::Vector2i(xx, yy));    
      }
    }
  }
}

void Forest::calculateTreeParaboloids(std::vector<TreeNode> &trees)
{
  std::vector<std::vector<Eigen::Vector3d> > point_lists(trees.size()); // in metres
  for (int x = 0; x<indexfield_.rows(); x++)
  {
    for (int y = 0; y<indexfield_.cols(); y++)
    {
      int ind = indexfield_(x, y);
      if (ind < 0)
        continue;
      while (ind >= 0)
      {
        point_lists[ind].push_back(Eigen::Vector3d(voxel_width_*((double)x + 0.5), voxel_width_*((double)y + 0.5), heightfield_(x, y)));
        ind = trees[ind].attaches_to;
      }
    }
  }
  for (size_t i = 0; i<trees.size(); i++)
  {
    auto &tree = trees[i];
    tree.approx_radius = voxel_width_ * std::sqrt((double)point_lists[i].size() / kPi);
    int x = (int)(tree.peak[0]/voxel_width_);
    int y = (int)(tree.peak[1]/voxel_width_);   
    x = std::max(0, std::min(x, (int)lowfield_.rows()-1));
    y = std::max(0, std::min(y, (int)lowfield_.cols()-1));
    tree.ground_height = lowfield_(x, y); 
    TreeNode::Node node;
    for (auto &pt: point_lists[i])
      node.add(pt[0], pt[1], pt[2], 1, voxel_width_);
    const int num_iterations = 10;
    for (int it = 1; it<num_iterations; it++)
    {
      node.abcd = node.curv_mat.ldlt().solve(node.curv_vec);
      node.curv_mat.setZero(); 
      node.curv_vec.setZero();
      for (auto &pt: point_lists[i])
      {
        double h = node.heightAt(pt[0], pt[1]);
        double error = h - pt[2];
        const double eps = 1e-2;
        node.add(pt[0], pt[1], pt[2], 1.0/std::max(eps, std::abs(error)), voxel_width_); // 1/e reweighting gives a median paraboloid
      }
    }
    node.abcd = node.curv_mat.ldlt().solve(node.curv_vec);

    tree.node = node;
  }
}

/*
bool Forest::save(const std::string &filename)
{
  std::ofstream ofs(filename.c_str(), std::ios::out);
  if (!ofs.is_open())
  {
    std::cerr << "Error: cannot open " << filename << " for writing." << std::endl;
    return false;
  }  
  ofs << "# Forest extraction, tree base location list: x, y, z, radius" << std::endl;
  for (auto &result: results_)
  {
    Eigen::Vector3d base = result.tree_tip * voxel_width_;
    base[2] = result.ground_height;
    const double tree_radius_to_trunk_radius = 1.0/20.0; // TODO: temporary until we have a better parameter choice
    ofs << base[0] << ", " << base[1] << ", " << base[2] << ", " << result.radius*tree_radius_to_trunk_radius << std::endl;
  }
  return true;
}*/
}
