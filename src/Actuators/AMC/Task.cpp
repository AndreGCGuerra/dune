//***************************************************************************
// Copyright 2007-2016 Universidade do Porto - Faculdade de Engenharia      *
// Laboratório de Sistemas e Tecnologia Subaquática (LSTS)                  *
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
// Author: Pedro Gonçalves                                                 *
//***************************************************************************

// DUNE headers.
#include <DUNE/DUNE.hpp>

//Local Headers
#include "MessageControl.hpp"

namespace Actuators
{
  namespace AMC
  {
    using DUNE_NAMESPACES;

    static const int c_max_csum = 2;
    static const int c_max_motors = 4;
    static const int c_max_buffer = 16;
    static const uint8_t c_poly = 0x00;
    static const int c_sleep_time = 250000;

    enum AmcMessages
    {
      // RPM
      RPM,
      // TEMPERATURE
      TEMPERATURE,
      // Voltage and Current
      PWR,
      // State of motor
      STATE
    };

    struct Arguments
    {
      //! Serial port device.
      std::string uart_dev;
      //! Serial port baud rate.
      unsigned uart_baud;
      //! Rpm entity labels
      std::string motor_elabels[c_max_motors];
      //! Internal conversion factors
      double internal_factors[c_max_motors];
      //! Motor state
      bool motor_state[c_max_motors];
    };

    struct Task: public DUNE::Tasks::Task
    {
      //! Rpm message
      IMC::Rpm m_rpm_val[c_max_motors];
      //! Task arguments.
      Arguments m_args;
      //!Func read name
      AmcMessages m_func_name;
      //! Serial port device.
      SerialPort* m_uart;
      // I/O Multiplexer.
      Poll m_poll;
      //! Scratch buffer.
      uint8_t m_buffer[c_max_buffer];
      //CSUM
      uint8_t m_csum[c_max_csum];
      //Buffer msg
      char m_msg[c_max_buffer];
      //Parser
      MessageParse* m_parse;
      //! Watchdog.
      Counter<double> m_wdog;
      //Counter stage id motor
      uint8_t m_cnt_motor;

      //! Constructor.
      //! @param[in] name task name.
      //! @param[in] ctx context.
      Task(const std::string& name, Tasks::Context& ctx):
        DUNE::Tasks::Task(name, ctx),
        m_uart(NULL)
      {
        // Define configuration parameters.
        param("Serial Port - Device", m_args.uart_dev)
        .defaultValue("")
        .description("Serial port device used to communicate with the sensor");

        param("Serial Port - Baud Rate", m_args.uart_baud)
        .defaultValue("57600")
        .description("Serial port baud rate");

        // Extract motor configurations
        for (unsigned i = 0; i < c_max_motors; ++i)
        {
          std::string option = String::str("Motor %u - Entity Label", i);
          param(option, m_args.motor_elabels[i])
          .defaultValue("")
          .description("Motor Entity Label");

          option = String::str("Motor %u - Conversion", i);
          param(option, m_args.internal_factors[i])
          .size(1)
          .defaultValue("1.0")
          .description("Motor rpm conversion factor");

          option = String::str("Motor %u - Conversion", i);
          param(option, m_args.motor_state[i])
          .defaultValue("true")
          .description("Motor State");
        }

        bind<IMC::SetThrusterActuation>(this);
      }

      //! Update internal state with new parameter values.
      void
      onUpdateParameters(void)
      {
        unsigned eid = 0;
        for (unsigned i = 0; i < c_max_motors; ++i)
        {
          try
          {
            eid = resolveEntity(m_args.motor_elabels[i]);
          }
          catch (...)
          { }

          if (m_args.motor_elabels[i].empty())
            continue;

          m_rpm_val[i].setSourceEntity(eid);
        }
      }

      //! Reserve entity identifiers.
      void
      onEntityReservation(void)
      {
        for (unsigned i = 0; i < c_max_motors; ++i)
        {
        try
          {
            resolveEntity(m_args.motor_elabels[i]);
          }
          catch (Entities::EntityDataBase::NonexistentLabel& e)
          {
            (void)e;
            reserveEntity(m_args.motor_elabels[i]);
          }
        }
      }

      //! Resolve entity names.
      void
      onEntityResolution(void)
      {
      }

      //! Acquire resources.
      void
      onResourceAcquisition(void)
      {
        setEntityState(IMC::EntityState::ESTA_NORMAL, Status::CODE_IDLE);
        m_uart = new SerialPort(m_args.uart_dev, m_args.uart_baud);
      }

      //! Initialize resources.
      void
      onResourceInitialization(void)
      {
        m_parse = new MessageParse();
        m_parse->m_amc_state = MessageParse::PS_PREAMBLE;
        m_poll.add(*m_uart);

        checkStateMotor();
        stopAllMotor();

        m_wdog.setTop(10.0);
      }

      //! Release resources.
      void
      onResourceRelease(void)
      {
        if (m_uart != NULL)
        {
          m_poll.remove(*m_uart);
          delete m_uart;
          m_uart = NULL;
        }
      }

      void
      consume(const IMC::SetThrusterActuation* msg)
      {
        if(msg->id == 0)
        {
          setRPM(0, msg->value);
          usleep(c_sleep_time);
          setRPM(1, msg->value);
          usleep(c_sleep_time);
        }
        else if(msg->id == 1)
        {
          setRPM(2, msg->value);
          usleep(c_sleep_time);
          setRPM(3, msg->value);
          usleep(c_sleep_time);
        }
      }

      //! Read input from arduino.
      void
      checkSerialPort(void)
      {
        if (m_poll.wasTriggered(*m_uart))
        {
          memset(&m_buffer, '\0', sizeof(m_buffer));
          int rv = 0;

          try
          {
            rv = m_uart->read(m_buffer, 1);
          }
          catch (std::exception& e)
          {
            err(DTR("read error: %s"), e.what());
            return;
          }

          if (rv <= 0)
          {
            err(DTR("unknown read error"));
            return;
          }
          else
            m_parse->ParserAMC(m_buffer[0]);
        }
      }

      int
      setRPM( int motor, int rpm )
      {
        Algorithms::CRC8 crc(c_poly);
        memset(&m_msg, '\0', sizeof(m_msg));
        sprintf(m_msg, "@S,%d,%d,*", motor, rpm);
        m_csum[0] = crc.putArray((unsigned char *)m_msg, strlen(m_msg) - 1);
        //m_csum[0] = m_parse->CRC8((unsigned char *)m_msg);
        int t = m_uart->write(m_msg, strlen(m_msg));
        m_uart->write(m_csum, 1);
        t++;
        //war("SEND: %s%c   SIZE: %d", m_msg, m_csum[0], t + 1);

        return t;
      }

      void
      checkStateMotor(void)
      {
        for(int i = 0; i < 4; i++)
        {
          readParameterAMC(i, STATE);
          usleep(c_sleep_time);
          if (m_poll.poll(0.5))
              checkSerialPort();
        }

        int cnt_war = 0;

        for(int i = 0; i < 4; i++)
        {
          if(m_parse->m_motor.state[i] == 0)
          {
            war(DTR("AMC Motor %d - ERROR"), i);
            cnt_war++;
          }
        }

        if(cnt_war > 0)
          setEntityState(IMC::EntityState::ESTA_ERROR, Utils::String::str(DTR("AMC Motor")));
      }

      void
      stopAllMotor(void)
      {
        setRPM(0, 0);
        usleep(c_sleep_time);
        setRPM(1, 0);
        usleep(c_sleep_time);
        setRPM(2, 0);
        usleep(c_sleep_time);
        setRPM(3, 0);
        usleep(c_sleep_time);
      }

      void
      readParameterAMC(int motor, AmcMessages _func_name)
      {
        Algorithms::CRC8 crc(c_poly);
        int t;
        if(_func_name == RPM)
        {
          memset(&m_msg, '\0', sizeof(m_msg));
          sprintf(m_msg, "@R,%d,rpm,*", motor);
          m_csum[0] = crc.putArray((unsigned char *)m_msg, strlen(m_msg) - 1);
          //m_csum[0] = m_parse->CRC8((unsigned char *)m_msg);
          t = m_uart->write(m_msg, strlen(m_msg));
          m_uart->write(m_csum, 1);
          t++;
        }
        else if(_func_name == TEMPERATURE)
        {
          memset(&m_msg, '\0', sizeof(m_msg));
          sprintf(m_msg, "@R,%d,tmp,*", motor);
          m_csum[0] = crc.putArray((unsigned char *)m_msg, strlen(m_msg) - 1);
          //m_csum[0] = m_parse->CRC8((unsigned char *)m_msg);
          t = m_uart->write(m_msg, strlen(m_msg));
          m_uart->write(m_csum, 1);
          t++;
        }
        else if(_func_name == PWR)
        {
          memset(&m_msg, '\0', sizeof(m_msg));
          sprintf(m_msg, "@R,%d,pwr,*", motor);
          m_csum[0] = crc.putArray((unsigned char *)m_msg, strlen(m_msg) - 1);
          //m_csum[0] = m_parse->CRC8((unsigned char *)m_msg);
          t = m_uart->write(m_msg, strlen(m_msg));
          m_uart->write(m_csum, 1);
          t++;
        }
        else if(_func_name == STATE)
        {
          memset(&m_msg, '\0', sizeof(m_msg));
          sprintf(m_msg, "@R,%d,sta,*", motor);
          m_csum[0] = crc.putArray((unsigned char *)m_msg, strlen(m_msg) - 1);
          //m_csum[0] = m_parse->CRC8((unsigned char *)m_msg);
          t = m_uart->write(m_msg, strlen(m_msg));
          m_uart->write(m_csum, 1);
          t++;
        }
        //war("SEND: %s%c   SIZE: %d", m_msg, m_csum[0], t + 1);
      }

      void
      dispatchRpm(int _id, int _rpm)
      {
        if(_id >= 0 && _id < c_max_motors)
        {
          m_rpm_val->setSourceEntity(resolveEntity(m_args.motor_elabels[_id]));
          m_rpm_val->value = _rpm;
          dispatch(m_rpm_val);
        }
      }

      //! Main loop.
      void
      onMain(void)
      {
        setEntityState(IMC::EntityState::ESTA_NORMAL, Status::CODE_ACTIVE);

        m_cnt_motor = 0;

        while (!stopping())
        {
          if (m_poll.poll(0.5))
          {
            checkSerialPort();
            m_wdog.reset();
          }
          else
          {
            readParameterAMC(m_cnt_motor, RPM);
            usleep(c_sleep_time);
            // Dispatch rpm motors.
            dispatchRpm(m_cnt_motor, (int)m_parse->m_motor.rpm[m_cnt_motor]);

            m_cnt_motor++;
            if(m_cnt_motor >= 4)
              m_cnt_motor = 0;

            waitForMessages(0.75);
            m_wdog.reset();

          }

          if (m_wdog.overflow())
          {
            setEntityState(IMC::EntityState::ESTA_ERROR, Utils::String::str(DTR("Watchdog Overflow")));
            throw RestartNeeded(DTR("Watchdog Overflow"), 2.0, false);
          }
        }
        stopAllMotor();
      }
    };
  }
}

DUNE_TASK