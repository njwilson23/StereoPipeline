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


/// \file StereoSession.cc
///
#include <vw/Core/Exception.h>
#include <vw/Core/Log.h>
#include <vw/Math/Vector.h>
#include <vw/Image/ImageViewRef.h>
#include <vw/Image/PixelMask.h>
#include <vw/Image/PixelTypeInfo.h>
#include <vw/FileIO/DiskImageResource.h>
#include <vw/FileIO/DiskImageView.h>

#include <asp/Core/StereoSettings.h>
#include <asp/Core/Common.h>
#include <asp/Sessions/StereoSession.h>
#include <asp/Core/InterestPointMatching.h>
#include <asp/Core/BundleAdjustUtils.h>
#include <asp/Camera/AdjustedLinescanDGModel.h>

#include <map>
#include <utility>
#include <string>
#include <ostream>
#include <limits>

using namespace vw;


/// Allows FileIO to correctly read/write these pixel types
namespace vw {
  template<> struct PixelFormatID<Vector3>   { static const PixelFormatEnum value = VW_PIXEL_GENERIC_3_CHANNEL; };
}

namespace asp {

  // Pass over all the string variables we use
  void StereoSession::initialize( BaseOptions const& options,
				  std::string const& left_image_file,
				  std::string const& right_image_file,
				  std::string const& left_camera_file,
				  std::string const& right_camera_file,
				  std::string const& out_prefix,
				  std::string const& input_dem) {
    m_options           = options;
    m_left_image_file   = left_image_file;
    m_right_image_file  = right_image_file;
    m_left_camera_file  = left_camera_file;
    m_right_camera_file = right_camera_file;
    m_out_prefix        = out_prefix;
    m_input_dem         = input_dem;
  }

  // A default IP matching implementation that derived classes can use
  bool StereoSession::ip_matching(std::string const& input_file1,
				  std::string const& input_file2,
				  vw::Vector2 const& uncropped_image_size,
				  Vector6f    const& stats1,
				  Vector6f    const& stats2,
				  int ip_per_tile,
				  float nodata1, float nodata2,
				  std::string const& match_filename,
				  vw::camera::CameraModel* cam1,
				  vw::camera::CameraModel* cam2){

    bool crop_left_and_right =
      ( stereo_settings().left_image_crop_win  != BBox2i(0, 0, 0, 0)) &&
      ( stereo_settings().right_image_crop_win != BBox2i(0, 0, 0, 0) );

    // If we crop the images we must always create new matching files
    if (!crop_left_and_right && boost::filesystem::exists(match_filename)) {
      vw_out() << "\t--> Using cached match file: " << match_filename << "\n";
      return true;
    }

    // Get normalized versions of the images for OpenCV based methods
    DiskImageView<float> image1(input_file1), image2(input_file2);
    ImageViewRef<float> image1_norm=image1, image2_norm=image2;
    if ( (stereo_settings().ip_matching_method != DETECT_IP_METHOD_INTEGRAL) &&
       (stats1[0] != stats1[1]) ) { // Don't normalize if no stats were provided!
      vw_out() << "\t--> Normalizing images for IP detection using stats " << stats1 << "\n";
      normalize_images(stereo_settings().force_use_entire_range,
		       stereo_settings().individually_normalize,
		       true, // Use percentile based stretch for ip matching
		       stats1,      stats2,
		       image1_norm, image2_norm);
    }

    const bool nadir_facing = this->is_nadir_facing();

    bool inlier = false;
    if (nadir_facing) {
      // Run an IP matching function that takes the camera and datum info into account
      bool single_threaded_camera = true; // TODO: This is probably needed only for ISIS.

      bool use_sphere_for_isis = false; // Assume Mars is not a sphere
      cartography::Datum datum = this->get_datum(cam1, use_sphere_for_isis);

      // Min % distance between closest and second closest descriptor matches
      const double match_seperation_threshold = 0.7;

      // This computes a distance used for throwing out interest points.
      // - It has to be computed using the entire (not cropped) image size!
      const double epipolar_threshold = norm_2(uncropped_image_size)/15;
      VW_OUT( DebugMessage, "asp" ) << "Epipolar threshold = " << epipolar_threshold << std::endl;

      inlier = ip_matching_w_alignment(single_threaded_camera, cam1, cam2,
				       image1_norm, image2_norm,
				       ip_per_tile,
				       datum, match_filename,
				       epipolar_threshold, match_seperation_threshold,
				       nodata1, nodata2);
    } else { // Not nadir facing
      // Run a simpler purely image based matching function
      inlier = homography_ip_matching( image1_norm, image2_norm,
				       ip_per_tile,
				       match_filename,
				       nodata1, nodata2);
    }
    if (!inlier) {
      boost::filesystem::remove(match_filename);
      vw_throw(IOErr() << "Unable to match left and right images.");
    }
    return inlier;
  }

  // Peek inside the images and camera models and return the datum and projection,
  // or at least the datum, packaged in a georef.
  vw::cartography::GeoReference StereoSession::get_georef() {

    vw::cartography::GeoReference georef;

    // First try to see if the image is map-projected.
    bool has_georef = read_georeference(georef, m_left_image_file);

    if (!has_georef) {
      // The best we can do is to get the datum, even non-projected
      // images have that. Create however a fake valid georeference to
      // go with this datum, otherwise we can't read the datum when we
      // needed it later.

      georef = vw::cartography::GeoReference();
      Matrix3x3 transform = georef.transform();
      transform(0,2) = 1;
      transform(1,2) = 1;
      georef.set_transform(transform);
      georef.set_geographic();

      boost::shared_ptr<vw::camera::CameraModel> cam = this->camera_model(m_left_image_file,
									  m_left_camera_file);
      bool use_sphere_for_isis = true;       // Spherical datum for non-Earth, as done usually
      georef.set_datum(this->get_datum(cam.get(), use_sphere_for_isis));
    }

    return georef;
  }

  // Default implementation of this function.  Derived classes will probably override this.
  void StereoSession::camera_models(boost::shared_ptr<vw::camera::CameraModel> &cam1,
				    boost::shared_ptr<vw::camera::CameraModel> &cam2) {
    cam1 = camera_model(m_left_image_file,  m_left_camera_file);
    cam2 = camera_model(m_right_image_file, m_right_camera_file);
  }

  // Processing Hooks. The default is to do nothing.
  void StereoSession::pre_preprocessing_hook(bool adjust_left_image_size,
					     std::string const& input_file1,
					     std::string const& input_file2,
					     std::string      & output_file1,
					     std::string      & output_file2) {
    output_file1 = input_file1;
    output_file2 = input_file2;
  }

  void StereoSession::post_preprocessing_hook(std::string const& input_file1,
					      std::string const& input_file2,
					      std::string &output_file1,
					      std::string &output_file2) {
    output_file1 = input_file1;
    output_file2 = input_file2;
  }

  void StereoSession::pre_correlation_hook(std::string const& input_file1,
					   std::string const& input_file2,
					   std::string      & output_file1,
					   std::string      & output_file2) {
    output_file1 = input_file1;
    output_file2 = input_file2;
  }

  void StereoSession::post_correlation_hook(std::string const& input_file,
					    std::string      & output_file) {
    output_file = input_file;
  }

  void StereoSession::pre_filtering_hook(std::string const& input_file,
					 std::string      & output_file) {
    output_file = input_file;
  }

  void StereoSession::post_filtering_hook(std::string const& input_file,
					  std::string      & output_file) {
    output_file = input_file;
  }

  ImageViewRef<PixelMask<Vector2f> >
  StereoSession::pre_pointcloud_hook(std::string const& input_file) {
    return DiskImageView<PixelMask<Vector2f> >( input_file );
  }

  void StereoSession::post_pointcloud_hook(std::string const& input_file,
					   std::string      & output_file) {
    output_file = input_file;
  }



  void StereoSession::get_nodata_values(boost::shared_ptr<vw::DiskImageResource> left_rsrc,
					boost::shared_ptr<vw::DiskImageResource> right_rsrc,
					float & left_nodata_value,
					float & right_nodata_value){

    // The no-data value read from options overrides the value present in the image files.
    left_nodata_value  = std::numeric_limits<float>::quiet_NaN();
    right_nodata_value = std::numeric_limits<float>::quiet_NaN();
    if (left_rsrc->has_nodata_read ()) left_nodata_value  = left_rsrc->nodata_read();
    if (right_rsrc->has_nodata_read()) right_nodata_value = right_rsrc->nodata_read();

    float opt_nodata = stereo_settings().nodata_value;
    if (!std::isnan(opt_nodata)){

      if ( opt_nodata < left_nodata_value )
	vw_out(WarningMessage) << "It appears that the user-supplied no-data value is less than the no-data value of left image. This may not be what was intended.\n";
      if ( opt_nodata < right_nodata_value )
	vw_out(WarningMessage) << "It appears that the user-supplied no-data value is less than the no-data value of right image. This may not be what was intended.\n";

      left_nodata_value  = opt_nodata;
      right_nodata_value = opt_nodata;
    }

    return;
  }

bool StereoSession::
shared_preprocessing_hook(asp::BaseOptions              & options,
                          std::string const             & left_input_file,
                          std::string const             & right_input_file,
                          std::string                   & left_output_file,
                          std::string                   & right_output_file,
                          std::string                   & left_cropped_file,
                          std::string                   & right_cropped_file,
                          float                         & left_nodata_value,
                          float                         & right_nodata_value,
                          bool                          & has_left_georef,
                          bool                          & has_right_georef,
                          vw::cartography::GeoReference & left_georef,
                          vw::cartography::GeoReference & right_georef){

  {
    // Retrieve nodata values and let the handles go out of scope right away.

    // For this to work the ISIS type must be registered with the
    // DiskImageResource class.  - This happens in "stereo.cc", so
    // these calls will create DiskImageResourceIsis objects.
    boost::shared_ptr<DiskImageResource>
      left_rsrc (DiskImageResource::open(left_input_file )),
      right_rsrc(DiskImageResource::open(right_input_file));
    this->get_nodata_values(left_rsrc, right_rsrc,
			    left_nodata_value, right_nodata_value);
  }

  // Set output file paths
  left_output_file  = this->m_out_prefix + "-L.tif";
  right_output_file = this->m_out_prefix + "-R.tif";

  left_cropped_file  = left_input_file;
  right_cropped_file = right_input_file;

  // Enforce no predictor in compression, it works badly with L.tif and R.tif.
  options = this->m_options;
  options.gdal_options["PREDICTOR"] = "1";

  // Read the georef if available
  has_left_georef  = read_georeference(left_georef,  left_cropped_file);
  has_right_georef = read_georeference(right_georef, right_cropped_file);
  if ( stereo_settings().alignment_method != "none") {
    // If any alignment at all happens, the georef will be messed up.
    has_left_georef = false;
    has_right_georef = false;
  }

  bool crop_left_and_right =
    ( stereo_settings().left_image_crop_win  != BBox2i(0, 0, 0, 0)) &&
    ( stereo_settings().right_image_crop_win != BBox2i(0, 0, 0, 0) );

  // If the output files already exist, and we don't crop both left
  // and right images, then there is nothing to do here.
  // Note: Must make sure all outputs are initialized before we
  // get to this part where we exit early.
  if ( boost::filesystem::exists(left_output_file)  &&
       boost::filesystem::exists(right_output_file) &&
       (!crop_left_and_right)) {
    try {
      vw_log().console_log().rule_set().add_rule(-1,"fileio");
      DiskImageView<PixelGray<float32> > out_left (left_output_file );
      DiskImageView<PixelGray<float32> > out_right(right_output_file);
      vw_out(InfoMessage) << "\t--> Using cached normalized input images.\n";
      vw_settings().reload_config();
      return true; // Return true if we exist early since the images exist
    } catch (vw::ArgumentErr const& e) {
      // This throws on a corrupted file.
      vw_settings().reload_config();
    } catch (vw::IOErr const& e) {
      vw_settings().reload_config();
    }
  } // End check for existing output files

  // See if to crop the images
  if (crop_left_and_right) {
    // Crop the images, will use them from now on. Crop the georef as
    // well, if available.
    left_cropped_file  = this->m_out_prefix + "-L-cropped.tif";
    right_cropped_file = this->m_out_prefix + "-R-cropped.tif";

    has_left_georef  = read_georeference(left_georef,  left_input_file);
    has_right_georef = read_georeference(right_georef, right_input_file);
    bool has_nodata = true;

    DiskImageView<float> left_orig_image(left_input_file);
    DiskImageView<float> right_orig_image(right_input_file);
    BBox2i left_win  = stereo_settings().left_image_crop_win;
    BBox2i right_win = stereo_settings().right_image_crop_win;
    left_win.crop (bounding_box(left_orig_image ));
    right_win.crop(bounding_box(right_orig_image));


    vw_out() << "\t--> Writing cropped image: " << left_cropped_file << "\n";
    block_write_gdal_image(left_cropped_file,
			   crop(left_orig_image, left_win),
			   has_left_georef, crop(left_georef, left_win),
			   has_nodata, left_nodata_value,
			   options,
			   TerminalProgressCallback("asp", "\t:  "));

    vw_out() << "\t--> Writing cropped image: " << right_cropped_file << "\n";
    block_write_gdal_image(right_cropped_file,
			   crop(right_orig_image, right_win),
			   has_right_georef,
			   crop(right_georef, right_win),
			   has_nodata, right_nodata_value,
			   options,
			   TerminalProgressCallback("asp", "\t:  "));
  }

  // Re-read the georef, since it changed above.
  has_left_georef  = read_georeference(left_georef,  left_cropped_file);
  has_right_georef = read_georeference(right_georef, right_cropped_file);
  if ( stereo_settings().alignment_method != "none") {
    // If any alignment at all happens, the georef will be messed up.
    has_left_georef = false;
    has_right_georef = false;
  }

  return false; // don't exit early
}

// TODO: Find a better place for these functions!

// If both left-image-crop-win and right-image-crop win are specified,
// we crop the images to these boxes, and hence the need to keep
// the upper-left corners of the crop windows to handle the cameras correctly.
vw::Vector2 camera_pixel_offset(std::string const& input_dem,
				       std::string const& left_image_file,
				       std::string const& right_image_file,
				       std::string const& curr_image_file){

  // For map-projected images we don't apply a pixel offset.
  // When we need to do stereo on cropped images, we just
  // crop the images together with their georeferences.
  if (input_dem != "")
    return Vector2();

  vw::Vector2 left_pixel_offset, right_pixel_offset;
  if ( ( stereo_settings().left_image_crop_win  != BBox2i(0, 0, 0, 0)) &&
       ( stereo_settings().right_image_crop_win != BBox2i(0, 0, 0, 0) ) ){
    left_pixel_offset  = stereo_settings().left_image_crop_win.min();
    right_pixel_offset = stereo_settings().right_image_crop_win.min();
  }
  if (curr_image_file == left_image_file)
    return left_pixel_offset;
  else if (curr_image_file == right_image_file)
    return right_pixel_offset;
  else
    // If the image files were not specified, no offset and no error.
    if ((left_image_file != "") || (right_image_file != ""))
      vw_throw(ArgumentErr() << "Supplied image file does not match left or right image file.");

  return Vector2();
}

// If we have adjusted camera models, load them.
boost::shared_ptr<vw::camera::CameraModel>
load_adjusted_model(boost::shared_ptr<vw::camera::CameraModel> cam,
		    std::string const& image_file,
		    std::string const& camera_file,
		    vw::Vector2 const& pixel_offset){

  // Any tool using adjusted camera models must pre-populate the
  // prefix at which to find them.
  std::string ba_pref = stereo_settings().bundle_adjust_prefix;
  if (ba_pref == "" && pixel_offset == vw::Vector2())
    return cam; // Just return if nothing is adjusting the camera

  std::vector<Vector3> position_correction;
  std::vector<Quat   > pose_correction;

  // Ensure these vectors are populated even when there are no corrections to read,
  // as we may still have pixel offset.
  position_correction.push_back(Vector3());
  pose_correction.push_back(Quat(math::identity_matrix<3>()));

  if (ba_pref != "") { // If a bundle adjustment file was specified

    // Get full BA file path
    std::string adjust_file = asp::bundle_adjust_file_name(ba_pref, image_file, camera_file);

    if (!boost::filesystem::exists(adjust_file))
      vw_throw(InputErr() << "Missing adjusted camera model: " << adjust_file << ".\n");

    vw_out() << "Using adjusted camera model: " << adjust_file << std::endl;
    bool piecewise_adjustments;
    Vector2 adjustment_bounds;
    asp::read_adjustments(adjust_file, piecewise_adjustments,
                          adjustment_bounds, position_correction, pose_correction);

    if (position_correction.empty() || pose_correction.empty())
      vw_throw(InputErr() << "Unable to read corrections.\n");

    // Handle the case of piecewise adjustments for DG cameras
    if (piecewise_adjustments) {

      // Create the adjusted DG model
      boost::shared_ptr<camera::CameraModel> adj_dg_cam
	(new AdjustedLinescanDGModel(cam,
                                     stereo_settings().piecewise_adjustment_interp_type,
                                     adjustment_bounds, position_correction, pose_correction));

      // Apply the pixel offset and pose corrections. So this a second adjustment
      // on top of the first.
      boost::shared_ptr<camera::CameraModel> adj_dg_cam2
	(new vw::camera::AdjustedCameraModel(adj_dg_cam, Vector3(),
					     Quat(math::identity_matrix<3>()), pixel_offset));

      return adj_dg_cam2;
    } // End case for piecewise DG adjustment

  } // End case for parsing bundle adjustment file

  // Create VW adjusted camera model object with the info we loaded
  return boost::shared_ptr<camera::CameraModel>(new vw::camera::AdjustedCameraModel
						(cam, position_correction[0],
						 pose_correction[0], pixel_offset));
}

} // End namespace asp
