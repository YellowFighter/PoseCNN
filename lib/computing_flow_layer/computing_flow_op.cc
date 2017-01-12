/* Copyright 2015 Google Inc. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

// Computing Flow Op

#include <stdio.h>
#include <cfloat>
#include <math.h> 

#include "third_party/eigen3/unsupported/Eigen/CXX11/Tensor"
#include "tensorflow/core/framework/op.h"
#include "tensorflow/core/framework/op_kernel.h"
#include "tensorflow/core/framework/tensor_shape.h"

using namespace tensorflow;
typedef Eigen::ThreadPoolDevice CPUDevice;

REGISTER_OP("Computeflow")
    .Attr("T: {float, double}")
    .Attr("kernel_size: int")
    .Attr("threshold: float")
    .Input("bottom_data: T")
    .Input("bottom_points: T")
    .Input("bottom_depth: T")
    .Input("bottom_meta_data: T")
    .Output("top_data: T")
    .Output("top_points: T");

REGISTER_OP("ComputeflowGrad")
    .Attr("T: {float, double}")
    .Attr("kernel_size: int")
    .Attr("threshold: float")
    .Input("bottom_data: T")
    .Input("bottom_points: T")
    .Input("top_points: T")
    .Input("grad: T")
    .Output("output: T");

template <typename Device, typename T>
class ComputeFlowOp : public OpKernel {
 public:
  explicit ComputeFlowOp(OpKernelConstruction* context) : OpKernel(context) {
    // Get the kernel size
    OP_REQUIRES_OK(context,
                   context->GetAttr("kernel_size", &kernel_size_));
    // Check that kernel size is positive
    OP_REQUIRES(context, kernel_size_ >= 0,
                errors::InvalidArgument("Need kernel_size >= 0, got ", kernel_size_));
    // Get the threshold
    OP_REQUIRES_OK(context,
                   context->GetAttr("threshold", &threshold_));
    // Check that threshold is positive
    OP_REQUIRES(context, threshold_ >= 0,
                errors::InvalidArgument("Need threshold >= 0, got ", threshold_));
  }

  // bottom_data: (batch_size, height, width, channels)
  void Compute(OpKernelContext* context) override 
  {
    // Grab the input tensor
    const Tensor& bottom_data = context->input(0);
    auto bottom_data_flat = bottom_data.flat<T>();

    const Tensor& bottom_points = context->input(1);
    auto im_points = bottom_points.flat<T>();

    const Tensor& bottom_depth = context->input(2);
    auto im_depth = bottom_depth.flat<T>();

    // format of the meta_data
    // intrinsic matrix: meta_data[0 ~ 8]
    // inverse intrinsic matrix: meta_data[9 ~ 17]
    // pose_world2live: meta_data[18 ~ 29]
    // pose_live2world: meta_data[30 ~ 41]
    // voxel step size: meta_data[42, 43, 44]
    // voxel min value: meta_data[45, 46, 47]
    const Tensor& bottom_meta_data = context->input(3);
    auto meta_data = bottom_meta_data.flat<T>();

    // data should have 4 dimensions.
    OP_REQUIRES(context, bottom_data.dims() == 4,
                errors::InvalidArgument("data must be 4-dimensional"));

    OP_REQUIRES(context, bottom_points.dims() == 4,
                errors::InvalidArgument("points must be 4-dimensional"));

    OP_REQUIRES(context, bottom_depth.dims() == 4,
                errors::InvalidArgument("depth must be 4-dimensional"));

    OP_REQUIRES(context, bottom_meta_data.dims() == 4,
                errors::InvalidArgument("meta data must be 4-dimensional"));

    // batch size
    int batch_size = bottom_data.dim_size(0);
    // height
    int height = bottom_data.dim_size(1);
    // width
    int width = bottom_data.dim_size(2);
    // number of channels
    int num_channels = bottom_data.dim_size(3);
    int num_meta_data = bottom_meta_data.dim_size(3);

    // Create output tensors
    // top_data
    int dims[4];
    dims[0] = batch_size;
    dims[1] = height;
    dims[2] = width;
    dims[3] = num_channels;
    TensorShape output_shape;
    TensorShapeUtils::MakeShape(dims, 4, &output_shape);

    Tensor* top_data_tensor = NULL;
    OP_REQUIRES_OK(context, context->allocate_output(0, output_shape, &top_data_tensor));
    auto top_data = top_data_tensor->template flat<T>();

    // top points
    dims[3] = 3;
    TensorShape output_shape_1;
    TensorShapeUtils::MakeShape(dims, 4, &output_shape_1);
    Tensor* top_points_tensor = NULL;
    OP_REQUIRES_OK(context, context->allocate_output(1, output_shape_1, &top_points_tensor));
    auto top_points = top_points_tensor->template flat<T>();

    int index_meta_data = 0;    
    for(int n = 0; n < batch_size; n++)
    {
      for(int h = 0; h < height; h++)
      {
        for(int w = 0; w < width; w++)
        {
          int index_pixel = n * height * width + h * width + w;

          // initialization
          for(int c = 0; c < num_channels; c++)
            top_data(index_pixel * num_channels + c) = 0;
          top_points(index_pixel * 3 + 0) = NAN;
          top_points(index_pixel * 3 + 1) = NAN;
          top_points(index_pixel * 3 + 2) = NAN;

          T depth = im_depth(index_pixel);
          if (depth > 0)
          {
            // backproject the pixel into 3D
            // apply the inverse intrinsic matrix
            T RX = meta_data(index_meta_data + 9) * w + meta_data(index_meta_data + 10) * h + meta_data(index_meta_data + 11);
            T RY = meta_data(index_meta_data + 12) * w + meta_data(index_meta_data + 13) * h + meta_data(index_meta_data + 14);
            T RZ = meta_data(index_meta_data + 15) * w + meta_data(index_meta_data + 16) * h + meta_data(index_meta_data + 17);

            // compute the 3D points in camera's coordinate system
            T X = depth * RX;
            T Y = depth * RY;
            T Z = depth * RZ;

            // apply pose_live2world
            T X1 = meta_data(index_meta_data + 30) * X + meta_data(index_meta_data + 31) * Y + meta_data(index_meta_data + 32) * Z + meta_data(index_meta_data + 33);
            T Y1 = meta_data(index_meta_data + 34) * X + meta_data(index_meta_data + 35) * Y + meta_data(index_meta_data + 36) * Z + meta_data(index_meta_data + 37);
            T Z1 = meta_data(index_meta_data + 38) * X + meta_data(index_meta_data + 39) * Y + meta_data(index_meta_data + 40) * Z + meta_data(index_meta_data + 41);

            top_points(index_pixel * 3 + 0) = X1;
            top_points(index_pixel * 3 + 1) = Y1;
            top_points(index_pixel * 3 + 2) = Z1;

            // check a neighborhood around (w, h)
            T dmin = 1000.0;
            int fx = -1;
            int fy = -1;
            for (int x = w - kernel_size_; x <= w + kernel_size_; x++)
            {
              for (int y = h - kernel_size_; y <= h + kernel_size_; y++)
              {
                if (x >= 0 && x < width && y >= 0 && y < height)
                {
                  int index = n * height * width + y * width + x;
                  T X_prev = im_points(index * 3 + 0);
                  T Y_prev = im_points(index * 3 + 1);
                  T Z_prev = im_points(index * 3 + 2);
                  if (std::isnan(X_prev) || std::isnan(Y_prev) || std::isnan(Z_prev))
                    continue;

                  // distance
                  T dis = sqrt((X1 - X_prev) * (X1 - X_prev) + (Y1 - Y_prev) * (Y1 - Y_prev) + (Z1 - Z_prev) * (Z1 - Z_prev));
                  if (dis < dmin)
                  {
                    dmin = dis;
                    fx = x;
                    fy = y;
                  }
                }
              }
            }

            if (dmin < threshold_)
            {
              // assign data
              int index = n * height * width + fy * width + fx;
              for(int c = 0; c < num_channels; c++)
                top_data(index_pixel * num_channels + c) = bottom_data_flat(index * num_channels + c);
            }
          }
        }
      }
      index_meta_data += num_meta_data;
    }
  }
 private:
  int kernel_size_;
  float threshold_;
};

REGISTER_KERNEL_BUILDER(Name("Computeflow").Device(DEVICE_CPU).TypeConstraint<float>("T"), ComputeFlowOp<CPUDevice, float>);
REGISTER_KERNEL_BUILDER(Name("Computeflow").Device(DEVICE_CPU).TypeConstraint<double>("T"), ComputeFlowOp<CPUDevice, double>);

bool ComputeFlowForwardLaucher(
    const float* bottom_data, const float* bottom_points,
    const float* bottom_depth, const float* bottom_meta_data,
    const int batch_size, const int height, const int width, const int channels, const int num_meta_data,
    const int kernel_size, const float threshold,
    float* top_data, float* top_points, const Eigen::GpuDevice& d);

static void ComputingFlowKernel(
    OpKernelContext* context, const Tensor* bottom_data, const Tensor* bottom_points,
    const Tensor* bottom_depth, const Tensor* bottom_meta_data,
    const int batch_size, const int height, const int width, const int channels, const int num_meta_data, 
    const int kernel_size, const float threshold, 
    const TensorShape& tensor_output_shape, const TensorShape& tensor_output_shape_points) 
{
  Tensor* top_data = nullptr;
  Tensor* top_points = nullptr;
  OP_REQUIRES_OK(context, context->allocate_output(0, tensor_output_shape, &top_data));
  OP_REQUIRES_OK(context, context->allocate_output(1, tensor_output_shape_points, &top_points));

  if (!context->status().ok()) {
    return;
  }

  ComputeFlowForwardLaucher(
    bottom_data->flat<float>().data(), bottom_points->flat<float>().data(),
    bottom_depth->flat<float>().data(), bottom_meta_data->flat<float>().data(),
    batch_size, height, width, channels, num_meta_data, kernel_size, threshold,
    top_data->flat<float>().data(), top_points->flat<float>().data(), context->eigen_device<Eigen::GpuDevice>());
}

template <class T>
class ComputeFlowOp<Eigen::GpuDevice, T> : public OpKernel {
 public:
  typedef Eigen::GpuDevice Device;

  explicit ComputeFlowOp(OpKernelConstruction* context) : OpKernel(context) {
    // Get the kernel size
    OP_REQUIRES_OK(context,
                   context->GetAttr("kernel_size", &kernel_size_));
    // Check that kernel size is positive
    OP_REQUIRES(context, kernel_size_ >= 0,
                errors::InvalidArgument("Need kernel_size >= 0, got ", kernel_size_));
    // Get the threshold
    OP_REQUIRES_OK(context,
                   context->GetAttr("threshold", &threshold_));
    // Check that threshold is positive
    OP_REQUIRES(context, threshold_ >= 0,
                errors::InvalidArgument("Need threshold >= 0, got ", threshold_));
  }

  void Compute(OpKernelContext* context) override 
  {
    // Grab the input tensor
    const Tensor& bottom_data = context->input(0);
    const Tensor& bottom_points = context->input(1);
    const Tensor& bottom_depth = context->input(2);
    const Tensor& bottom_meta_data = context->input(3);

    // data should have 4 dimensions.
    OP_REQUIRES(context, bottom_data.dims() == 4,
                errors::InvalidArgument("data must be 4-dimensional"));

    OP_REQUIRES(context, bottom_points.dims() == 4,
                errors::InvalidArgument("label must be 4-dimensional"));

    OP_REQUIRES(context, bottom_depth.dims() == 4,
                errors::InvalidArgument("depth must be 4-dimensional"));

    OP_REQUIRES(context, bottom_meta_data.dims() == 4,
                errors::InvalidArgument("meta data must be 4-dimensional"));

    // batch size
    int batch_size = bottom_data.dim_size(0);
    // height
    int height = bottom_data.dim_size(1);
    // width
    int width = bottom_data.dim_size(2);
    // Number of channels
    int num_channels = bottom_data.dim_size(3);
    int num_meta_data = bottom_meta_data.dim_size(3);

    // Create output tensors
    // top_data
    int dims[4];
    dims[0] = batch_size;
    dims[1] = height;
    dims[2] = width;
    dims[3] = num_channels;
    TensorShape output_shape;
    TensorShapeUtils::MakeShape(dims, 4, &output_shape);

    // top points
    dims[3] = 3;
    TensorShape output_shape_points;
    TensorShapeUtils::MakeShape(dims, 4, &output_shape_points);
    
    ComputingFlowKernel(context, &bottom_data, &bottom_points, &bottom_depth, &bottom_meta_data, batch_size, height,
      width, num_channels, num_meta_data, kernel_size_, threshold_, output_shape, output_shape_points);
  }
 private:
  int kernel_size_;
  float threshold_;
};

REGISTER_KERNEL_BUILDER(Name("Computeflow").Device(DEVICE_GPU).TypeConstraint<float>("T"), ComputeFlowOp<Eigen::GpuDevice, float>);


bool ComputeFlowBackwardLaucher(const float* top_diff, const float* bottom_points, const float* top_points, const int batch_size,
    const int height, const int width, const int channels, const int kernel_size, const float threshold,
    float* bottom_diff, const Eigen::GpuDevice& d);

static void ComputingFlowGradKernel(
    OpKernelContext* context, const Tensor* bottom_points, const Tensor* top_points, const Tensor* out_backprop,
    const int batch_size, const int height, const int width, const int channels, const int kernel_size, const float threshold,
    const TensorShape& tensor_output_shape) 
{
  Tensor* output = nullptr;
  OP_REQUIRES_OK(context, context->allocate_output(0, tensor_output_shape, &output));

  if (!context->status().ok()) {
    return;
  }

  ComputeFlowBackwardLaucher(
    out_backprop->flat<float>().data(), bottom_points->flat<float>().data(), top_points->flat<float>().data(),
    batch_size, height, width, channels, kernel_size, threshold, output->flat<float>().data(), context->eigen_device<Eigen::GpuDevice>());
}


// compute gradient
template <class Device, class T>
class ComputeFlowGradOp : public OpKernel {
 public:
  explicit ComputeFlowGradOp(OpKernelConstruction* context) : OpKernel(context) {
    // Get the kernel size
    OP_REQUIRES_OK(context,
                   context->GetAttr("kernel_size", &kernel_size_));
    // Check that kernel size is positive
    OP_REQUIRES(context, kernel_size_ >= 0,
                errors::InvalidArgument("Need kernel_size >= 0, got ", kernel_size_));
    // Get the threshold
    OP_REQUIRES_OK(context,
                   context->GetAttr("threshold", &threshold_));
    // Check that threshold is positive
    OP_REQUIRES(context, threshold_ >= 0,
                errors::InvalidArgument("Need threshold >= 0, got ", threshold_));
  }

  void Compute(OpKernelContext* context) override 
  {
    // Grab the input tensor
    const Tensor& bottom_data = context->input(0);
    const Tensor& bottom_points = context->input(1);
    const Tensor& top_points = context->input(2);
    const Tensor& out_backprop = context->input(3);

    // data should have 4 dimensions.
    OP_REQUIRES(context, bottom_data.dims() == 4,
                errors::InvalidArgument("data must be 4-dimensional"));

    OP_REQUIRES(context, bottom_points.dims() == 4,
                errors::InvalidArgument("bottom points must be 4-dimensional"));

    OP_REQUIRES(context, top_points.dims() == 4,
                errors::InvalidArgument("top points must be 4-dimensional"));

    // batch size
    int batch_size = bottom_data.dim_size(0);
    // height
    int height = bottom_data.dim_size(1);
    // width
    int width = bottom_data.dim_size(2);
    // number of channels
    int num_channels = bottom_data.dim_size(3);

    // construct the output shape
    TensorShape output_shape = bottom_data.shape();

    ComputingFlowGradKernel(
      context, &bottom_points, &top_points, &out_backprop,
      batch_size, height, width, num_channels, kernel_size_, threshold_, output_shape);

  }
 private:
  int kernel_size_;
  float threshold_;
};

REGISTER_KERNEL_BUILDER(Name("ComputeflowGrad").Device(DEVICE_GPU).TypeConstraint<float>("T"), ComputeFlowGradOp<Eigen::GpuDevice, float>);