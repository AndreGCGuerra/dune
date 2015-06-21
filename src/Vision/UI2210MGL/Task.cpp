//***************************************************************************
// Copyright 2013-2015 Norwegian University of Science and Technology (NTNU)*
// Centre for Autonomous Marine Operations and Systems (AMOS)               *
// Department of Engineering Cybernetics (ITK)                              *
//***************************************************************************
// This file is part of DUNE: Unified Navigation Environment.               *
//                                                                          *
// Commercial Licence Usage                                                 *
// Licencees holding valid commercial DUNE licences may use this file in    *
// accordance with the commercial licence agreement provided with the       *
// Software or, alternatively, in accordance with the terms contained in a  *
// written agreement between you and Universidade do Porto. For licensing   *
// terms, conditions, and further information contact lsts@fe.up.pt.        *
//                                                                          *
// European Union Public Licence - EUPL v.1.1 Usage                         *
// Alternatively, this file may be used under the terms of the EUPL,        *
// Version 1.1 only (the "Licence"), appearing in the file LICENCE.md       *
// included in the packaging of this file. You may not use this work        *
// except in compliance with the Licence. Unless required by applicable     *
// law or agreed to in writing, software distributed under the Licence is   *
// distributed on an "AS IS" basis, WITHOUT WARRANTIES OR CONDITIONS OF     *
// ANY KIND, either express or implied. See the Licence for the specific    *
// language governing permissions and limitations at                        *
// http://ec.europa.eu/idabc/eupl.html.                                     *
//***************************************************************************
// Author: Torkel Hansen                                                    *
// Author: João Fortuna                                                     *
//***************************************************************************

// ISO C++ 98 headers.
#include <queue>
#include <string>

// DUNE headers.
#include <DUNE/DUNE.hpp>

#include <opencv2/imgproc/imgproc.hpp>
#include <opencv2/core/core.hpp>
#include <opencv2/highgui/highgui.hpp>

#if defined(DUNE_SYS_HAS_OPENCV2_IMGCODECS_HPP)
#  include <opencv2/imgcodecs.hpp>
#endif


// Local headers.
#include "CaptureUeye.hpp"

using DUNE_NAMESPACES;

namespace Vision
{
  //! Device driver for the uEye UI-2210-M-GL USB Camera.
  //!
  //! @author Torkel Hansen
  //! @author João Fortuna
  namespace UI2210MGL
  {
    //! %Task arguments.
    struct Arguments
    {
      //! Frames Per Second.
      unsigned fps;
      //! Compression quality.
      unsigned quality;
      //! Path to log directory
      std::string log_dir;
      //! Area of Interest specification
      AOI aoi;
      //! Auto Gain
      bool auto_gain;
      //! Calibration parameters
      double c1, c2, c3;
      //! Wavelength to send
      float wlen;
    };

    //! Device driver task.
    struct Task: public DUNE::Tasks::Task
    {
      //! %Frame width. Unclear if 640 or 960
      static const unsigned c_width = 640;
      //! %Frame height. 480 is total, 250 is usable.
      static const unsigned c_height = 250;
      //! Configuration parameters.
      Arguments m_args;
      //! %Destination log folder.
      Path m_log_dir;
      //! Camera handle.
      HIDS m_cam;
      //! Flag to allow ignoring the first run.
      bool m_starting;
      //! Thread for image capture.
      CaptureUeye* m_capture;
      //! OpenCV frame.
      cv::Mat m_image_cv;
      //! Wavelengths.
//      float m_wlengths[3];

      Task(const std::string& name, Tasks::Context& ctx):
        Tasks::Task(name, ctx),
        m_log_dir(ctx.dir_log),
        m_cam(1),
        m_starting(true),
        m_capture(NULL)
      {
        // Retrieve configuration values.
        paramActive(Tasks::Parameter::SCOPE_MANEUVER,
                    Tasks::Parameter::VISIBILITY_USER);

        param("Frames Per Second", m_args.fps)
        .defaultValue("30")
        .minimumValue("0")
        .maximumValue("75")
        .description("Frames per second");

        param("AOI - X", m_args.aoi.x)
        .defaultValue("0")
        .minimumValue("0")
        .description("X coordinate of upper left corner of AOI");

        param("AOI - Y", m_args.aoi.y)
        .defaultValue("0")
        .minimumValue("0")
        .description("Y coordinate of upper left corner of AOI");

        param("AOI - Width", m_args.aoi.width)
        .defaultValue("0")
        .minimumValue("0")
        .description("Width of AOI");

        param("AOI - Height", m_args.aoi.height)
        .defaultValue("0")
        .minimumValue("0")
        .description("Height of AOI");

        param("Quality", m_args.quality)
        .defaultValue("80")
        .units(Units::Percentage)
        .minimumValue("0")
        .maximumValue("100")
        .description("Image compression quality");

        param("Auto Gain", m_args.auto_gain)
        .defaultValue("false")
        .description("Enable Auto Gain");

        param("Calib - C1", m_args.c1)
        .defaultValue("372")
        .description("C1 parameter of polynomial calibration function");

        param("Calib - C2", m_args.c2)
        .defaultValue("0.505")
        .description("C2 parameter of polynomial calibration function");

        param("Calib - C3", m_args.c3)
        .defaultValue("-0.00002235")
        .description("C3 parameter of polynomial calibration function");

        param("Live Wavelength", m_args.wlen)
        .defaultValue("500.0")
        .description("Send live data from this wavelength");

        param("Log Dir", m_args.log_dir)
        .defaultValue("")
        .description("Path to Log Directory");

        bind<IMC::LoggingControl>(this);
//        bind<IMC::HyperSnapRequest>(this);
      }

      //! Update internal parameters.
      void
      onUpdateParameters(void)
      {
        if (m_starting)
        {
          m_starting = false;
          return;
        }

        m_log_dir = m_args.log_dir;

        m_capture->setAOI(m_args.aoi);
        m_capture->setFPS(m_args.fps);
        m_capture->setAutoGain(m_args.auto_gain);
      }

      //! Acquire resources and buffers.
      void
      onResourceAcquisition(void)
      {
        m_capture = new CaptureUeye(this, m_args.aoi, m_cam, m_args.fps);
        m_capture->start();
        m_capture->setAutoGain(m_args.auto_gain);
      }

      //! Release allocated resources.
      void
      onResourceRelease(void)
      {
        if (m_capture != NULL)
        {
          delete m_capture;
          m_capture = NULL;
        }
      }

      //! Initialize resources and start capturing frames.
      void
      onResourceInitialization(void)
      {
        setEntityState(IMC::EntityState::ESTA_NORMAL, Status::CODE_IDLE);
      }

      void
      consume(const IMC::LoggingControl* msg)
      {
        if (!isActivating() && (msg->getDestination() != getSystemId()))
          return;

        if (msg->op == IMC::LoggingControl::COP_CURRENT_NAME)
        {
          m_log_dir = m_args.log_dir / msg->name;
          m_log_dir.create();
        }
      }

      void
      onRequestActivation(void)
      {
        IMC::LoggingControl log_ctl;
        log_ctl.op = IMC::LoggingControl::COP_REQUEST_CURRENT_NAME;
        dispatch(log_ctl);
      }

//      void
//      onActivation(void)
//      {
//        m_log_dir.create();
//        setEntityState(IMC::EntityState::ESTA_NORMAL, Status::CODE_ACTIVE);
//        m_capture->start();
//      }
//
//      void
//      onDeactivation(void)
//      {
//        setEntityState(IMC::EntityState::ESTA_NORMAL, Status::CODE_IDLE);
//      }

      //! Saves the image.
      void
      saveImage(Frame* frame)
      {
        Path file = m_log_dir / String::str("%0.4f_%d.bmp", frame->timestamp, frame->gain_factor);

        m_image_cv = cv::Mat(m_args.aoi.height, m_args.aoi.width, CV_8UC1);
        std::memcpy(m_image_cv.ptr(), frame->data, m_args.aoi.height * m_args.aoi.width);

        cv::flip(m_image_cv, m_image_cv, 0);
        cv::imwrite(file.c_str(), m_image_cv);
      }

      float
      pixel2wlen(int pix)
      {
        // Polynomial conversion function
        return (float) (m_args.c1 + m_args.c2 * pix + m_args.c3 * pix * pix);
      }

      int
      wlen2pixel(float wlen)
      {
        // Inverse polynomial conversion function
        return (int) ((-m_args.c2 + std::sqrt(m_args.c2 * m_args.c2 - 4 * m_args.c3 * (m_args.c1 - wlen))) / (2 * m_args.c3));
      }

//      void
//      consume(const IMC::HyperSnapRequest* msg)
//      {
//        std::vector<std::string> vec;
//        String::split(msg->wavelengths, ",", vec);
//
//        for (int i = 0; i < vec.size(); i++)
//          m_wlengths[i] = std::atof(vec[i].c_str());
//      }

      void
      sendData(float wlen, int gain, double timestamp)
      {
        IMC::HyperSpecData hyper;

        hyper.setTimeStamp(timestamp);

        cv::Mat slice = m_image_cv.col(wlen2pixel(wlen));
        hyper.data.assign(slice.data, slice.data+slice.rows);
        hyper.gain = (float)gain / 100;
        hyper.wavelen = wlen;

        dispatch(hyper);
      }

      void
      onMain(void)
      {
        Frame* frame = NULL;

        while (!stopping()) // && isActive())
        {
          consumeMessages();

          frame = m_capture->readFrame();
          if (frame == NULL)
            Time::Delay::wait(0.5);
          else
          {
            saveImage(frame);
            sendData(m_args.wlen, frame->gain_factor, frame->timestamp);
          }
        }

        m_capture->stopCapture();

        bool qhasdata = true;
        int i = -1;

        while (qhasdata)
        {
          inf("Emptying buffer.");
          frame = m_capture->readFrame();
          if (frame == NULL)
            qhasdata = false;
          else
            saveImage(frame);
          i++;
        }

        inf("%d images in buffer at shutdown.", i);

        m_capture->stopAndJoin();

        delete frame;
      }
    };
  }
}

DUNE_TASK
