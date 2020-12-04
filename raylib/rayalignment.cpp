// Copyright (c) 2020
// Commonwealth Scientific and Industrial Research Organisation (CSIRO)
// ABN 41 687 119 230
//
// Author: Thomas Lowe
#include "rayalignment.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "imagewrite.h"

#include "simple_fft/fft.h"

#include <cinttypes>
#include <iostream>
#include <complex>

using Complex = std::complex<double>;
static const double kHighPassPower = 0.25;  // This fixes inout->inout11, inoutD->inoutB2 and house_inside->house3.
                                            // Doesn't break any. power=0.25. 0 is turned off.
namespace ray
{
struct Array3D
{
  void init(const Eigen::Vector3d &box_min, const Eigen::Vector3d &box_max, double voxel_width);

  void fft();
  void inverseFft();

  void operator*=(const Array3D &other);

  inline Complex &operator()(int x, int y, int z) { return cells_[x + dims_[0] * y + dims_[0] * dims_[1] * z]; }
  inline const Complex &operator()(int x, int y, int z) const { return cells_[x + dims_[0] * y + dims_[0] * dims_[1] * z]; }
  inline Complex &operator()(const Eigen::Vector3i &index) { return (*this)(index[0], index[1], index[2]); }
  inline const Complex &operator()(const Eigen::Vector3i &index) const { return (*this)(index[0], index[1], index[2]); }
  Complex &operator()(const Eigen::Vector3d &pos)
  {
    Eigen::Vector3d index = (pos - box_min_) / voxel_width_;
    if (index[0] >= 0.0 && index[1] >= 0.0 && index[2] >= 0.0 && index[0] < (double)dims_[0] &&
        index[1] < (double)dims_[1] && index[2] < (double)dims_[2])
      return (*this)(Eigen::Vector3i(index.cast<int>()));
    return null_cell_;
  }
  const Complex &operator()(const Eigen::Vector3d &pos) const
  {
    Eigen::Vector3d index = (pos - box_min_) / voxel_width_;
    if (index[0] >= 0.0 && index[1] >= 0.0 && index[2] >= 0.0 && index[0] < (double)dims_[0] &&
        index[1] < (double)dims_[1] && index[2] < (double)dims_[2])
      return (*this)(Eigen::Vector3i(index.cast<int>()));
    return null_cell_;
  }
  void conjugate();
  Eigen::Vector3i maxRealIndex() const;
  void fillWithRays(const Cloud &cloud);
  inline Eigen::Vector3i &dimensions(){ return dims_; }
  inline const Eigen::Vector3i &dimensions() const { return dims_; }
  inline double voxelWidth(){ return voxel_width_; }
  void clearCells(){ cells_.clear(); }
private:
  Eigen::Vector3d box_min_, box_max_;
  double voxel_width_;
  Eigen::Vector3i dims_;
  std::vector<Complex> cells_;
  Complex null_cell_;
};

struct Array1D
{
  void init(int length);
  void fft();
  void inverseFft();

  void operator*=(const Array1D &other);
  inline Complex &operator()(const int &x) { return cells_[x]; }
  inline const Complex &operator()(const int &x) const { return cells_[x]; }
  inline void operator+=(const Array1D &other)
  {
    for (int i = 0; i < (int)cells_.size(); i++) cells_[i] += other.cells_[i];
  }
  void polarCrossCorrelation(const Array3D *arrays, bool verbose);

  int maxRealIndex() const;
  void conjugate();
  int numCells(){ return (int)cells_.size(); }
  Complex &cell(int i){ return cells_[i]; }
  const Complex &cell(int i) const { return cells_[i]; }
private:
  std::vector<Complex> cells_;
};

struct Col
{
  uint8_t r, g, b, a;
};

void Array3D::init(const Eigen::Vector3d &box_min, const Eigen::Vector3d &box_max, double voxel_width)
{
  box_min_ = box_min;
  box_max_ = box_max;
  voxel_width_ = voxel_width;
  Eigen::Vector3d diff = (box_max - box_min) / voxel_width;
  // HERE we need to make it a power of two
  for (int i = 0; i < 3; i++) 
    dims_[i] = 1 << (int)ceil(log2(ceil(diff[i])));  // next power of two larger than diff
  cells_.resize(dims_[0] * dims_[1] * dims_[2]);
  memset(&cells_[0], 0, cells_.size() * sizeof(Complex));
  null_cell_ = 0;
}

void Array3D::operator*=(const Array3D &other)
{
  for (int i = 0; i < (int)cells_.size(); i++) 
    cells_[i] *= other.cells_[i];
}

void Array3D::conjugate()
{
  for (int i = 0; i < (int)cells_.size(); i++) 
    cells_[i] = conj(cells_[i]);
}

void Array3D::fft()
{
  const char *error = nullptr;
  // TODO: change to in-place (cells not repeated)
  if (!simple_fft::FFT(*this, *this, dims_[0], dims_[1], dims_[2], error))
    std::cout << "failed to calculate FFT: " << error << std::endl;
}

void Array3D::inverseFft()
{
  const char *error = nullptr;
  if (!simple_fft::IFFT(*this, *this, dims_[0], dims_[1], dims_[2], error))
    std::cout << "failed to calculate inverse FFT: " << error << std::endl;
}

Eigen::Vector3i Array3D::maxRealIndex() const
{
  Eigen::Vector3i index;
  double highest = std::numeric_limits<double>::lowest();
  for (int i = 0; i < (int)cells_.size(); i++)
  {
    const double &score = cells_[i].real();
    if (score > highest)
    {
      index = Eigen::Vector3i(i % dims_[0], (i / dims_[0]) % dims_[1], i / (dims_[0] * dims_[1]));
      highest = score;
    }
  }
  return index;
}

void Array3D::fillWithRays(const Cloud &cloud)
{
  // unlike the end point densities, the weight is just 0 or 1, but requires walking through the grid for every ray
  // maybe a better choice would be a reuseable 'volume' function (occupancy grid).
  for (int i = 0; i < (int)cloud.ends.size(); i++)
  {
    // TODO: Should be a lambda function!
    Eigen::Vector3d dir = cloud.ends[i] - cloud.starts[i];
    Eigen::Vector3d dir_sign(sgn(dir[0]), sgn(dir[1]), sgn(dir[2]));
    Eigen::Vector3d start = (cloud.starts[i] - box_min_) / voxel_width_;
    Eigen::Vector3d end = (cloud.ends[i] - box_min_) / voxel_width_;
    Eigen::Vector3i start_index(start.cast<int>());
    Eigen::Vector3i end_index(end.cast<int>());
    double length_sqr = (end_index - start_index).squaredNorm();
    Eigen::Vector3i index = start_index;
    while ((index - start_index).squaredNorm() <= length_sqr + 1e-10)
    {
      if (index[0] >= 0 && index[0] < dims_[0] && index[1] >= 0 && index[1] < dims_[1] && index[2] >= 0 &&
          index[2] < dims_[2])
        (*this)(index[0], index[1], index[2]) += Complex(1, 0);  // add weight to these areas...

      Eigen::Vector3d mid = box_min_ + voxel_width_ * Eigen::Vector3d(index[0] + 0.5, index[1] + 0.5, index[2] + 0.5);
      Eigen::Vector3d next_boundary = mid + 0.5 * voxel_width_ * dir_sign;
      Eigen::Vector3d delta = next_boundary - cloud.starts[i];
      Eigen::Vector3d d(delta[0] / dir[0], delta[1] / dir[1], delta[2] / dir[2]);
      if (d[0] < d[1] && d[0] < d[2])
        index[0] += dir_sign.cast<int>()[0];
      else if (d[1] < d[0] && d[1] < d[2])
        index[1] += dir_sign.cast<int>()[1];
      else
        index[2] += dir_sign.cast<int>()[2];
    }
  }
}

/**************************************************************************************************/

void Array1D::init(int length)
{
  cells_.resize(length);
  memset(&cells_[0], 0, cells_.size() * sizeof(Complex));
}

void Array1D::operator*=(const Array1D &other)
{
  for (int i = 0; i < (int)cells_.size(); i++) 
    cells_[i] *= other.cells_[i];
}

void Array1D::conjugate()
{
  for (int i = 0; i < (int)cells_.size(); i++) 
    cells_[i] = conj(cells_[i]);
}

void Array1D::fft()
{
  const char *error = nullptr;
  // TODO: change to in-place (cells not repeated)
  if (!simple_fft::FFT(*this, *this, cells_.size(), error))
    std::cout << "failed to calculate FFT: " << error << std::endl;
}

void Array1D::inverseFft()
{
  const char *error = nullptr;
  if (!simple_fft::IFFT(*this, *this, cells_.size(), error))
    std::cout << "failed to calculate inverse FFT: " << error << std::endl;
}

int Array1D::maxRealIndex() const
{
  int index = 0;
  double highest = std::numeric_limits<double>::lowest();
  for (int i = 0; i < (int)cells_.size(); i++)
  {
    const double &score = cells_[i].real();
    if (score > highest)
    {
      index = i;
      highest = score;
    }
  }
  return index;
}

void drawArray(const Array3D &array, const Eigen::Vector3i &dims, const std::string &file_name, int index)
{
  int width = dims[0];
  int height = dims[1];
  double max_val = 0.0;
  for (int x = 0; x < width; x++)
  {
    for (int y = 0; y < height; y++)
    {
      double val = 0.0;
      for (int z = 0; z < dims[2]; z++) val += abs(array(x, y, z));
      max_val = std::max(max_val, val);
    }
  }

  std::vector<Col> pixels(width * height);
  for (int x = 0; x < width; x++)
  {
    for (int y = 0; y < height; y++)
    {
      Eigen::Vector3d colour(0, 0, 0);
      for (int z = 0; z < dims[2]; z++)
      {
        double h = (double)z / (double)dims[2];
        Eigen::Vector3d col;
        col[0] = 1.0 - h;
        col[2] = h;
        col[1] = 3.0 * col[0] * col[2];
        colour += std::abs(array(x, y, z)) * col;
      }
      colour *= 15.0 * 255.0 / max_val;
      Col col;
      col.r = uint8_t(clamped((int)colour[0], 0, 255));
      col.g = uint8_t(clamped((int)colour[1], 0, 255));
      col.b = uint8_t(clamped((int)colour[2], 0, 255));
      col.a = 255;
      pixels[(x + width / 2) % width + width * ((y + height / 2) % height)] = col;
    }
  }
  std::stringstream str;
  str << file_name << index << ".png";
  stbi_write_png(str.str().c_str(), width, height, 4, (void *)&pixels[0], 4 * width);
}

void drawArray(const std::vector<Array1D> &arrays, const Eigen::Vector3i &dims, const std::string &file_name, int index)
{
  int width = dims[0];
  int height = dims[1];
  double max_val = 0.0;
  for (int x = 0; x < width; x++)
  {
    for (int y = 0; y < height; y++)
    {
      double val = 0.0;
      for (int z = 0; z < dims[2]; z++) val += abs(arrays[y + dims[1] * z](x));
      max_val = std::max(max_val, val);
    }
  }

  std::vector<Col> pixels(width * height);
  for (int x = 0; x < width; x++)
  {
    for (int y = 0; y < height; y++)
    {
      Eigen::Vector3d colour(0, 0, 0);
      for (int z = 0; z < dims[2]; z++)
      {
        double h = (double)z / (double)dims[2];
        Eigen::Vector3d col;
        col[0] = 1.0 - h;
        col[2] = h;
        col[1] = 3.0 * col[0] * col[2];
        colour += std::abs(arrays[y + dims[1] * z](x)) * col;
      }
      colour *= 3.0 * 255.0 / max_val;
      Col col;
      col.r = uint8_t(clamped((int)colour[0], 0, 255));
      col.g = uint8_t(clamped((int)colour[1], 0, 255));
      col.b = uint8_t(clamped((int)colour[2], 0, 255));
      col.a = 255;
      pixels[(x + width / 2) % width + width * y] = col;
    }
  }
  std::stringstream str;
  str << file_name << index << ".png";
  stbi_write_png(str.str().c_str(), width, height, 4, (void *)&pixels[0], 4 * width);
}

void Array1D::polarCrossCorrelation(const Array3D *arrays, bool verbose)
{
  // OK cool, so next I need to re-map the two arrays into 4x1 grids...
  int max_rad = std::max(arrays[0].dimensions()[0], arrays[0].dimensions()[1]) / 2;
  Eigen::Vector3i polar_dims = Eigen::Vector3i(4 * max_rad, max_rad, arrays[0].dimensions()[2]);
  std::vector<Array1D> polars[2];
  for (int c = 0; c < 2; c++)
  {
    std::vector<Array1D> &polar = polars[c];
    const Array3D &a = arrays[c];
    polar.resize(polar_dims[1] * polar_dims[2]);
    for (int j = 0; j < polar_dims[1]; j++)
      for (int k = 0; k < polar_dims[2]; k++) polar[j + polar_dims[1] * k].init(polar_dims[0]);

    // now map...
    for (int i = 0; i < polar_dims[0]; i++)
    {
      double angle = 2.0 * kPi * (double)(i + 0.5) / (double)polar_dims[0];
      for (int j = 0; j < polar_dims[1]; j++)
      {
        double radius = (0.5 + (double)j) / (double)polar_dims[1];
        Eigen::Vector2d pos = radius * 0.5 * Eigen::Vector2d((double)a.dimensions()[0] * sin(angle), (double)a.dimensions()[1] * cos(angle));
        if (pos[0] < 0.0)
          pos[0] += a.dimensions()[0];
        if (pos[1] < 0.0)
          pos[1] += a.dimensions()[1];
        int x = pos.cast<int>()[0];
        int y = pos.cast<int>()[1];
        int x2 = (x + 1) % a.dimensions()[0];
        int y2 = (y + 1) % a.dimensions()[1];
        double blend_x = pos[0] - (double)x;
        double blend_y = pos[1] - (double)y;
        for (int z = 0; z < polar_dims[2]; z++)
        {
          // bilinear interpolation -- for some reason LERP after abs is better than before abs
          double val = abs(a(x, y, z)) * (1.0 - blend_x) * (1.0 - blend_y) +
                       abs(a(x2, y, z)) * blend_x * (1.0 - blend_y) + abs(a(x, y2, z)) * (1.0 - blend_x) * blend_y +
                       abs(a(x2, y2, z)) * blend_x * blend_y;
          polar[j + polar_dims[1] * z](i) = Complex(radius * val, 0);
        }
      }
    }
    if (verbose)
      drawArray(polar, polar_dims, "translationInvPolar", c);
    for (int j = 0; j < polar_dims[1]; j++)
    {
      for (int k = 0; k < polar_dims[2]; k++)
      {
        int i = j + polar_dims[1] * k;
        polar[i].fft();
        if (kHighPassPower > 0.0)
        {
          for (int l = 0; l < polar[i].numCells(); l++)
            polar[i].cell(l) *= std::pow(std::min((double)l, (double)(polar[i].numCells() - l)), kHighPassPower);
        }
      }
    }
    if (verbose)
      drawArray(polar, polar_dims, "euclideanInvariant", c);
  }

  // now get the inverse fft in place:
  init(polar_dims[0]);
  for (size_t i = 0; i < polars[0].size(); i++)
  {
    polars[1][i].conjugate();
    polars[0][i] *= polars[1][i];
    polars[0][i].inverseFft();
    (*this) += polars[0][i];  // add all the results together into the first array
  }
}

/************************************************************************************/
void alignCloud0ToCloud1(Cloud *clouds, double voxel_width, bool verbose)
{
  // first we need to decimate the clouds into intensity grids..
  // I need to get a maximum box width, and individual box_min, boxMaxs
  Eigen::Vector3d box_mins[2], box_width(0, 0, 0);
  for (int c = 0; c < 2; c++)
  {
    const double mx = std::numeric_limits<double>::max();
    const double mn = std::numeric_limits<double>::lowest();
    Eigen::Vector3d box_min(mx,mx,mx), box_max(mn,mn,mn);
    for (int i = 0; i < (int)clouds[c].ends.size(); i++)
    {
      if (clouds[c].rayBounded(i))
      {
        box_min = minVector(box_min, clouds[c].ends[i]);
        box_max = maxVector(box_max, clouds[c].ends[i]);
      }
    }
    box_mins[c] = box_min;
    Eigen::Vector3d width = box_max - box_min;
    box_width = maxVector(box_width, width);
  }

  bool rotation_to_estimate = true;  // If we know there is no rotation between the clouds then we can save some cost

  Array3D arrays[2];
  // Now fill in the arrays with point density
  for (int c = 0; c < 2; c++)
  {
    arrays[c].init(box_mins[c], box_mins[c] + box_width, voxel_width);
    for (int i = 0; i < (int)clouds[c].ends.size(); i++)
      if (clouds[c].rayBounded(i))
        arrays[c](clouds[c].ends[i]) += Complex(1, 0);
    arrays[c].fft();
    if (verbose)
      drawArray(arrays[c], arrays[c].dimensions(), "translationInvariant", c);
  }

  if (rotation_to_estimate)
  {
    Array1D polar;
    polar.polarCrossCorrelation(arrays, verbose);

    // get the angle of rotation
    int index = polar.maxRealIndex();
    // add a little bit of sub-pixel accuracy:
    double angle;
    int dim = polar.numCells();
    int back = (index + dim - 1) % dim;
    int fwd = (index + 1) % dim;
    double y0 = polar(back).real();
    double y1 = polar(index).real();
    double y2 = polar(fwd).real();
    angle = index + 0.5 * (y0 - y2) / (y0 + y2 - 2.0 * y1);  // just a quadratic maximum -b/2a for heights y0,y1,y2
    // but the FFT wraps around, so:
    if (angle > dim / 2)
      angle -= dim;
    angle *= 2.0 * kPi / (double)polar.numCells();
    std::cout << "estimated yaw rotation: " << angle << std::endl;

    // ok, so let's rotate A towards B, and re-run the translation FFT
    Pose pose(Eigen::Vector3d(0, 0, 0), Eigen::Quaterniond(Eigen::AngleAxisd(angle, Eigen::Vector3d(0, 0, 1))));
    clouds[0].transform(pose, 0.0);

    const double mx = std::numeric_limits<double>::max();
    box_mins[0] = Eigen::Vector3d(mx,mx,mx);
    for (int i = 0; i < (int)clouds[0].ends.size(); i++)
      if (clouds[0].rayBounded(i))
        box_mins[0] = minVector(box_mins[0], clouds[0].ends[i]);
    arrays[0].clearCells();
    arrays[0].init(box_mins[0], box_mins[0] + box_width, voxel_width);

    for (int i = 0; i < (int)clouds[0].ends.size(); i++)
      if (clouds[0].rayBounded(i))
        arrays[0](clouds[0].ends[i]) += Complex(1, 0);

    arrays[0].fft();
    if (verbose)
      drawArray(arrays[0], arrays[0].dimensions(), "translationInvariantWeighted", 0);
  }

  if (kHighPassPower > 0.0)
  {
    for (int c = 0; c < 2; c++)
    {
      for (int x = 0; x < arrays[c].dimensions()[0]; x++)
      {
        double coord_x = x < arrays[c].dimensions()[0] / 2 ? x : arrays[c].dimensions()[0] - x;
        for (int y = 0; y < arrays[c].dimensions()[1]; y++)
        {
          double coord_y = y < arrays[c].dimensions()[1] / 2 ? y : arrays[c].dimensions()[1] - y;
          for (int z = 0; z < arrays[c].dimensions()[2]; z++)
          {
            double coord_z = z < arrays[c].dimensions()[2] / 2 ? z : arrays[c].dimensions()[2] - z;
            arrays[c](x, y, z) *= pow(sqr(coord_x) + sqr(coord_y) + sqr(coord_z), kHighPassPower);
          }
        }
      }
      if (verbose)
        drawArray(arrays[c], arrays[c].dimensions(), "normalised", c);
    }
  }
  /****************************************************************************************************/
  // now get the the translation part
  arrays[1].conjugate();
  arrays[0] *= arrays[1];
  arrays[0].inverseFft();

  // find the peak
  Array3D &array = arrays[0];
  Eigen::Vector3i ind = array.maxRealIndex();
  // add a little bit of sub-pixel accuracy:
  Eigen::Vector3d pos;
  for (int axis = 0; axis < 3; axis++)
  {
    Eigen::Vector3i back = ind, fwd = ind;
    int &dim = array.dimensions()[axis];
    back[axis] = (ind[axis] + dim - 1) % dim;
    fwd[axis] = (ind[axis] + 1) % dim;
    double y0 = array(back).real();
    double y1 = array(ind).real();
    double y2 = array(fwd).real();
    pos[axis] =
      ind[axis] + 0.5 * (y0 - y2) / (y0 + y2 - 2.0 * y1);  // just a quadratic maximum -b/2a for heights y0,y1,y2
    // but the FFT wraps around, so:
    if (pos[axis] > dim / 2)
      pos[axis] -= dim;
  }
  pos *= -array.voxelWidth();
  pos += box_mins[1] - box_mins[0];
  std::cout << "estimated translation: " << pos.transpose() << std::endl;

  Pose transform(pos, Eigen::Quaterniond::Identity());
  clouds[0].transform(transform, 0.0);
}

void alignCloudToAxes(Cloud &cloud)
{
  // Idea 1: Calculate normals, find 2 largest densities of orthogonal normals
  // Idea 2: Calculate normals, each normal add complex c = e^4*angle, angle of total c is aligned angle
  // Idea 3: Idea 2, but repeat reweighting normals away from best guess (i.e. robust)
  // But normals don't work on vineyard rows. We need densities (histograms) in each direction...
  // Idea 4: 2D Hough transform, for each point draw circle, the brightest point indicates a line, 
  //         we then find the brightest orthogonal point
  // Idea 5: 3D Hough transform, to find the brightest corner (L shape)

  // I think I'll go with idea 4, it works on noisy data too.

  // first decimate the cloud (I'll do this later)
 
  Eigen::Vector3d min_bound = cloud.calcMinBound();
  Eigen::Vector3d max_bound = cloud.calcMaxBound();
  Eigen::Vector3d mid = (min_bound + max_bound)/2.0;
  Eigen::Vector3d centroid(0,0,0);
  for (size_t e = 0; e<cloud.ends.size(); e++)
  {
    if (!cloud.rayBounded(e))
      continue;
    centroid += cloud.ends[e];
  }
  centroid /= (double)cloud.ends.size();

  const int ang_res = 256, amp_res = 256; // ang_res must be divisible by 2
  double weights[ang_res][amp_res];
  std::memset(weights, 0, sizeof(double)*ang_res*amp_res);
  double radius = 0.5 * std::sqrt(2.0) * std::max(max_bound[0]-min_bound[0], max_bound[1]-min_bound[1]);
  double eps = 0.0001;

  // first convert the cloud into a weighted centroid field. I'm using element [2] for the weight.
  Eigen::Vector3d ps[amp_res][amp_res];
  std::memset(ps, 0, sizeof(Eigen::Vector3d)*amp_res*amp_res);
  double step_x = ((double)amp_res-1.0-eps) / (max_bound[0]-min_bound[0]);
  double step_y = ((double)amp_res-1.0-eps) / (max_bound[1]-min_bound[1]);
  for (size_t e = 0; e<cloud.ends.size(); e++)
  {
    if (!cloud.rayBounded(e))
      continue;
    Eigen::Vector3d index = cloud.ends[e] - min_bound;
    index[0] *= step_x;
    index[1] *= step_y;
    Eigen::Vector3d pos = cloud.ends[e] - mid;
    pos[2] = 1.0;
    ps[(int)index[0]][(int)index[1]] += pos;
  }

  for (int ii = 0; ii<amp_res; ii++)
  {
    for (int jj = 0; jj<amp_res; jj++)
    {
      Eigen::Vector3d &p = ps[ii][jj];
      double w = p[2];
      if (w == 0.0)
        continue;
      Eigen::Vector2d pos(p[0]/w, p[1]/w);
      double angle = atan2(pos[0], pos[1]);
      double amp = std::sqrt(pos[0]*pos[0] + pos[1]*pos[1]) / radius;

      // now draw the sine wave for this point. (should it be anti-aliased?)
      for (int i = 0; i<ang_res; i++)
      {
        double ang = kPi * (double)i/(double)ang_res;
        double y = amp * std::sin(ang + angle);
        double x = ((double)amp_res-1.0-eps) * (0.5 + 0.5*y);
        int j = (int)x;
        double blend = x - (double)j;
        
        weights[i][j] += (1.0-blend)*w;
        weights[i][j+1] += blend*w;
      }
    } 
  }
  // now find heighest weight:    
  int max_i = 0, max_j = 0;
  double max_weight = 0.0;
  for (int i = 0; i<ang_res; i++)
  {
    for (int j = 0; j<amp_res; j++)
    {
      if (weights[i][j] > max_weight)
      {
        max_i = i;
        max_j = j;
        max_weight = weights[i][j];
      }
    }
  }
  std::cout << "max_i: " << max_i << ", max_j: " << max_j << std::endl;

  // now interpolate the location
  double x0 = weights[(max_i + ang_res-1)%ang_res][max_j];
  double x1 = weights[max_i][max_j];
  double x2 = weights[(max_i+1)%ang_res][max_j];
  double angle = (double)max_i+0.5 + 0.5 * (x0 - x2) / (x0 + x2 - 2.0 * x1);  // just a quadratic maximum -b/2a for heights y0,y1,y2
  angle *= kPi / (double)ang_res;
  std::cout << "principle angle: " << angle << ", direction vec: " << std::cos(angle) << ", " << std::sin(angle) << std::endl;

  double y0 = weights[max_i][std::max(0, max_j-1)];
  double y1 = weights[max_i][max_j];
  double y2 = weights[max_i][std::min(max_j+1, amp_res-1)];
  double amp = (double)max_j+0.5 + 0.5 * (y0 - y2) / (y0 + y2 - 2.0 * y1);  // just a quadratic maximum -b/2a for heights y0,y1,y2
  amp = radius * ((2.0 * amp/(double)amp_res) - 1.0);
  std::cout << "distance along direction: " << amp << ", radius: " << radius << "maxj/ampres: " << max_j/(double)amp_res << std::endl;

  Eigen::Vector2d line_vector = amp * Eigen::Vector2d(std::cos(angle), std::sin(angle));

  // now find the orthogonal best edge. Simply the greatest weight along the orthogonal angle
  int orth_i = (max_i + ang_res/2)%ang_res;
  double max_w = 0.0;
  int max_orth_j = 0;
  for (int j = 0; j<amp_res; j++)
  {
    if (weights[orth_i][j] > max_w)
    {
      max_w = weights[orth_i][j];
      max_orth_j = j;
    }
  }
  // now interpolate this orthogonal direction:
  double z0 = weights[orth_i][std::max(0, max_orth_j-1)];
  double z1 = weights[orth_i][max_orth_j];
  double z2 = weights[orth_i][std::min(max_orth_j+1, amp_res-1)];
  double amp2 = (double)max_orth_j+0.5 + 0.5 * (z0 - z2) / (z0 + z2 - 2.0 * z1);  // just a quadratic maximum -b/2a for heights y0,y1,y2
  amp2 = radius * ((2.0 * amp2/(double)amp_res) - 1.0);
  std::cout << "orthi: " << orth_i << ", max_i: " << max_i << std::endl;
  if (orth_i < max_i) // the %res earlier puts it in antiphase (since we only have 180 degrees per weights map)
    amp2 = -amp2;     // so negate the amplitude here

  Eigen::Vector2d line2_vector = amp2 * Eigen::Vector2d(std::cos(angle+kPi/2.0), std::sin(angle+kPi/2.0));
  std::cout << "line2_vector: " << line2_vector.transpose() << std::endl;
  std::cout << "amp: " << amp << ", amp2: " << amp2 << std::endl;

  // great, we now have the angle and cross-over position. Next is to flip it so the largest side is positive
  Eigen::Vector2d centre = line_vector + line2_vector;
  Eigen::Quaterniond id(1,0,0,0);
  Pose to_mid(-mid - Eigen::Vector3d(centre[0], centre[1], 0), id);
  Pose rot(Eigen::Vector3d::Zero(), Eigen::Quaterniond(Eigen::AngleAxisd(-angle, Eigen::Vector3d(0,0,1))));
  Pose pose = rot * to_mid;

  // now rotate the cloud into the positive quadrant
  Eigen::Vector3d mid_point = pose * centroid;
  if (mid_point[0] < 0.0)
    pose = Pose(Eigen::Vector3d(0,0,0), Eigen::Quaterniond(0,0,0,1)) * pose; // 180 degree yaw

  // lastly move cloud vertically based on densest point. 
  double ws[amp_res]; // TODO: height is usually much less than width... more constant voxel size?
  memset(ws, 0, sizeof(double)*amp_res);
  double step_z = ((double)amp_res - 1.0 - eps) / (max_bound[2] - min_bound[2]);
  for (size_t i = 0; i<cloud.ends.size(); i++)
  {
    if (!cloud.rayBounded(i))
      continue;
    double z = ((cloud.ends[i][2] - min_bound[2]) * step_z);
    int k = (int)z;
    double blend = z - (double)k;
    ws[k] += (1.0 - blend);
    ws[k+1] += blend;
  }
  int max_k = 0;
  double max_wt = 0;
  for (int k = 0; k<amp_res; k++)
  {
    if (ws[k] > max_wt) 
    {
      max_wt = ws[k];
      max_k = k;
    }
  }
  double w0 = ws[std::max(0, max_k-1)];
  double w1 = ws[max_k];
  double w2 = ws[std::min(max_k+1, amp_res-1)];
  double k2 = (double)max_k+0.5 + 0.5 * (w0 - w2) / (w0 + w2 - 2.0 * w1);  // just a quadratic maximum -b/2a for heights y0,y1,y2
  double height = (k2 / step_z) + min_bound[2];
  pose.position[2] = -height;

  std::cout << "pose: " << pose.position.transpose() << ", q: " << pose.rotation.x() << ", " << pose.rotation.y() << ", " << pose.rotation.z() << std::endl;

  cloud.transform(pose, 0);
}

} // namespace ray
