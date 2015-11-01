// __BEGIN_LICENSE__
//  Copyright (c) 2009-2013, United States Government as represented by the
//  Administrator of the National Aeronautics and Space Administration. All
//  rights reserved.
//
//  The NGT platform is licensed under the Apache License, Version 2.0 (the
//  "License"); you may not use this file except in compliance with the
//  License. You may obtain a copy of the License at
//  http://www.apache.org/licenses/LICENSE-2.0
//
//  Unless required by applicable law or agreed to in writing, software
//  distributed under the License is distributed on an "AS IS" BASIS,
//  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//  See the License for the specific language governing permissions and
//  limitations under the License.
// __END_LICENSE__


/// \file StereoSessionDG.h
///
/// This a session to support Digital Globe images from Quickbird and World View.

#ifndef __STEREO_SESSION_DG_H__
#define __STEREO_SESSION_DG_H__

#include <asp/Sessions/StereoSessionConcrete.h>
#include <vw/Stereo/StereoModel.h>

#include <asp/Core/StereoSettings.h>
#include <asp/Core/InterestPointMatching.h>
#include <asp/Core/AffineEpipolar.h>
#include <asp/Camera/RPCModel.h>
#include <asp/Camera/DG_XML.h>

namespace asp {


  /// Generic stereoSession implementation for images which we can read/write with GDAL.
  /// - This class adds a "preprocessing hook" which aligns and normalizes the images using the specified methods.
  template <STEREOSESSION_DISKTRANSFORM_TYPE  DISKTRANSFORM_TYPE,
	    STEREOSESSION_STEREOMODEL_TYPE    STEREOMODEL_TYPE>
  class StereoSessionGdal : public StereoSessionConcrete<DISKTRANSFORM_TYPE, STEREOMODEL_TYPE> {

  public:
    StereoSessionGdal(){}
    virtual ~StereoSessionGdal(){}

    virtual std::string name() const { return "dg"; }

    /// Stage 1: Preprocessing
    ///
    /// Pre file is a pair of images.            ( ImageView<PixelT> )
    /// Post file is a pair of grayscale images. ( ImageView<PixelGray<float> > )
    virtual inline void pre_preprocessing_hook(bool adjust_left_image_size,
					std::string const& left_input_file,
					std::string const& right_input_file,
					std::string      & left_output_file,
					std::string      & right_output_file);

    /// Simple factory function
    static StereoSession* construct() { return new StereoSessionGdal<DISKTRANSFORM_TYPE, STEREOMODEL_TYPE>; }
  };

  /// StereoSession implementation for Digital Globe images.
  typedef StereoSessionGdal<DISKTRANSFORM_TYPE_MATRIX, STEREOMODEL_TYPE_DG> StereoSessionDG;


// Function definitions

  template <STEREOSESSION_DISKTRANSFORM_TYPE  DISKTRANSFORM_TYPE,
	    STEREOSESSION_STEREOMODEL_TYPE    STEREOMODEL_TYPE>
  inline void StereoSessionGdal<DISKTRANSFORM_TYPE, STEREOMODEL_TYPE>::
  pre_preprocessing_hook(bool adjust_left_image_size,
			 std::string const& left_input_file,
			 std::string const& right_input_file,
			 std::string &left_output_file,
			 std::string &right_output_file) {

    std::string left_cropped_file, right_cropped_file;
    asp::BaseOptions options;
    float left_nodata_value, right_nodata_value;
    bool has_left_georef, has_right_georef;
    vw::cartography::GeoReference left_georef, right_georef;
    bool exit_early =
      StereoSession::shared_preprocessing_hook(options,
					     left_input_file,   right_input_file,
					     left_output_file,  right_output_file,
					     left_cropped_file, right_cropped_file,
					     left_nodata_value, right_nodata_value,
					     has_left_georef,   has_right_georef,
					     left_georef,       right_georef);

    if (exit_early) return;

    // Load the cropped images
    DiskImageView<float> left_disk_image (left_cropped_file ),
    right_disk_image(right_cropped_file);

    // Set up image masks
    ImageViewRef< PixelMask<float> > left_masked_image
      = create_mask_less_or_equal(left_disk_image,  left_nodata_value);
    ImageViewRef< PixelMask<float> > right_masked_image
      = create_mask_less_or_equal(right_disk_image, right_nodata_value);

    // Compute input image statistics
    Vector6f left_stats  = gather_stats(left_masked_image,  "left" );
    Vector6f right_stats = gather_stats(right_masked_image, "right");

    ImageViewRef< PixelMask<float> > Limg, Rimg;
    std::string lcase_file = boost::to_lower_copy(this->m_left_camera_file);

    // Image alignment block - Generate aligned versions of the input
    // images according to the options.
    if ( stereo_settings().alignment_method == "homography" ||
	 stereo_settings().alignment_method == "affineepipolar" ) {
      // Define the file name containing IP match information.
      std::string match_filename = ip::match_filename(this->m_out_prefix,
						      left_cropped_file,
						      right_cropped_file);

      // Detect matching interest points between the left and right input images.
      // - The output is written directly to file!
      DiskImageView<float> left_orig_image(left_input_file);
      boost::shared_ptr<camera::CameraModel> left_cam, right_cam;
      this->camera_models(left_cam, right_cam); // Fetch the camera models.
      this->ip_matching(left_cropped_file,   right_cropped_file,
			bounding_box(left_orig_image).size(),
			left_stats, right_stats,
			stereo_settings().ip_per_tile,
			left_nodata_value, right_nodata_value, match_filename,
			left_cam.get(),    right_cam.get() );

      // Load the interest points results from the file we just wrote.
      std::vector<ip::InterestPoint> left_ip, right_ip;
      ip::read_binary_match_file(match_filename, left_ip, right_ip);

      // Initialize alignment matrices and get the input image sizes.
      Matrix<double> align_left_matrix  = math::identity_matrix<3>(),
		     align_right_matrix = math::identity_matrix<3>();
      Vector2i left_size  = file_image_size(left_cropped_file ),
	       right_size = file_image_size(right_cropped_file);

      // Compute the appropriate alignment matrix based on the input points
      if ( stereo_settings().alignment_method == "homography" ) {
	left_size = homography_rectification(adjust_left_image_size,
					     left_size,         right_size,
					     left_ip,           right_ip,
					     align_left_matrix, align_right_matrix);
	vw_out() << "\t--> Aligning right image to left using matrices:\n"
		 << "\t      " << align_left_matrix  << "\n"
		 << "\t      " << align_right_matrix << "\n";
      } else {
	left_size = affine_epipolar_rectification(left_size,         right_size,
						  left_ip,           right_ip,
						  align_left_matrix, align_right_matrix);
	vw_out() << "\t--> Aligning left and right images using affine matrices:\n"
		 << "\t      " << submatrix(align_left_matrix, 0,0,2,3) << "\n"
		 << "\t      " << submatrix(align_right_matrix,0,0,2,3) << "\n";
      }
      // Write out both computed matrices to disk
      write_matrix(this->m_out_prefix + "-align-L.exr", align_left_matrix );
      write_matrix(this->m_out_prefix + "-align-R.exr", align_right_matrix);

      // Apply the alignment transform to both input images
      Limg = transform(left_masked_image,
		       HomographyTransform(align_left_matrix),
		       left_size.x(), left_size.y() );
      Rimg = transform(right_masked_image,
		       HomographyTransform(align_right_matrix),
		       left_size.x(), left_size.y() );
    } else if ( stereo_settings().alignment_method == "epipolar" ) {
      vw_throw( NoImplErr() << "StereoSessionGdal does not support epipolar rectification" );
    } else {
      // No alignment, just provide the original files.
      Limg = left_masked_image;
      Rimg = right_masked_image;
    } // End of image alignment block

    // Apply our normalization options.
    normalize_images(stereo_settings().force_use_entire_range,
		     stereo_settings().individually_normalize,
		     false, // Use std stretch
		     left_stats, right_stats, Limg, Rimg);

    // The output no-data value must be < 0 as we scale the images to [0, 1].
    bool has_nodata = true;
    float output_nodata = -32768.0;

    // The left image is written out with no alignment warping.
    vw_out() << "\t--> Writing pre-aligned images.\n";
    block_write_gdal_image( left_output_file, apply_mask(Limg, output_nodata),
			    has_left_georef, left_georef,
			    has_nodata, output_nodata, options,
			    TerminalProgressCallback("asp","\t  L:  ") );

    if ( stereo_settings().alignment_method == "none" )
      block_write_gdal_image( right_output_file, apply_mask(Rimg, output_nodata),
			      has_right_georef, right_georef,
			      has_nodata, output_nodata, options,
			      TerminalProgressCallback("asp","\t  R:  ") );
    else // Write out the right image cropped to align with the left image.
      block_write_gdal_image( right_output_file,
			      apply_mask(crop(edge_extend(Rimg, ConstantEdgeExtension()),
					      bounding_box(Limg)), output_nodata),
			      has_right_georef, right_georef,
			      has_nodata, output_nodata, options,
			      TerminalProgressCallback("asp","\t  R:  ") );
  } // End function pre_preprocessing_hook



} // End namespace asp

#endif//__STEREO_SESSION_DG_H__
