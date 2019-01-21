#include <sys/mman.h>
#include <cmath>
#include <time.h>
#include <signal.h>
#include <stdexcept>

// ROS headers
#include <ros/ros.h>
#include <controller_manager/controller_manager.h>
#include <std_msgs/Duration.h>
#include <std_msgs/Float64MultiArray.h>
#include <hardware_interface/joint_state_interface.h>
#include <hardware_interface/joint_command_interface.h>
#include <joint_limits_interface/joint_limits.h>
#include <joint_limits_interface/joint_limits_interface.h>
#include <joint_limits_interface/joint_limits_rosparam.h>
#include <joint_limits_interface/joint_limits_urdf.h>
#include <control_toolbox/filters.h>
#include <urdf/model.h>

// qb tools
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include "qbmove_communications.h"
#include "soft_hand_ros_control/definitions.h"

#include <utility>
#include <map>

// just thinking of any time in the future the soft hand might have more than one synergy
#define N_SYN 1

bool g_quit = false;

void quitRequested(int sig) {
  g_quit = true;
}

namespace soft_hand_hw
{ 

  class GloveWrapper {

    private:

      std::string fifo_path_;
      FILE* fd_;

      

      double fs_[11]; // Finger state from thumb to little finger (including time stamp, which is the last value)


    public:

      //typedef std::Ptr<GloveWrapper> Ptr;


      GloveWrapper(const std::string& fifo_path = "/tmp/obi_glove_device_fifo"){

        fifo_path_ = fifo_path;

        fd_ = fopen(fifo_path_.c_str(),"r");

      }

     ~GloveWrapper(){
       fclose(fd_);
     }


      void read(double* state){

        fscanf(fd_,"%lf, %lf, %lf, %lf, %lf, %lf, %lf, %lf, %lf, %lf, %lf",&fs_[0], &fs_[1], &fs_[2], &fs_[3], &fs_[4], 
                                            &fs_[5], &fs_[6], &fs_[7], &fs_[8], &fs_[9], &fs_[10]);
        memcpy(state, fs_, sizeof(double)*11);

      }


  };


  class SHHWSensorised : public hardware_interface::RobotHW
  {
  public:
    // from RobotHW
    SHHWSensorised(ros::NodeHandle nh);
    bool start();
    bool read(ros::Time time, ros::Duration period);
    void write(ros::Time time, ros::Duration period);
    void stop();
    void set_mode();
    void reset();
    void registerJointLimits(const std::string& joint_name,
                           const hardware_interface::JointHandle& joint_handle,
                           const urdf::Model *const urdf_model,
                           double *const lower_limit, double *const upper_limit, 
                           double *const effort_limit);

    // from QBtools
    // port selection by id
    int port_selection(const int id, char* my_port);
    int open_port(char*);
    void set_input(short int);

    // structure for a lwr, joint handles, low lever interface, etc
    struct SHRDevice
    {
      // configuration
      std::vector<std::string> joint_names;

      // limits
      std::vector<double> 
        joint_lower_limits,
        joint_upper_limits,
        joint_effort_limits;

      // state and commands
      std::vector<double>
        joint_position,
        joint_position_prev,
        joint_velocity,
        joint_effort,
        joint_position_command;

      void init(int n_dof = N_SYN)
      {
        joint_position.resize(n_dof);
        joint_position_prev.resize(n_dof);
        joint_velocity.resize(n_dof);
        joint_effort.resize(n_dof);
        joint_position_command.resize(N_SYN);
 
        joint_lower_limits.resize(n_dof);
        joint_upper_limits.resize(n_dof);
        joint_effort_limits.resize(n_dof);
      }

      // reset values
      void reset() 
      {
        for (int j = 0; j < joint_position.size(); ++j)
        {
          joint_position[j] = 0.0;
          joint_position_prev[j] = 0.0;
          joint_velocity[j] = 0.0;
          joint_effort[j] = 0.0;
        }

        for (int j = 0; j < N_SYN; ++j)
        {
          joint_position_command[j] = 0.0;
        }
      }



    };

    boost::shared_ptr<SHHWSensorised::SHRDevice> device_;

  private:

    // Node handle
    ros::NodeHandle nh_;

    // QB tools Parameters
    int device_id_;
    comm_settings comm_settings_t_;
    char port_[255];

    urdf::Model urdf_model_;

    // interfaces, qb tools only allow position interface, AFAIK
    hardware_interface::JointStateInterface state_interface_;
    hardware_interface::PositionJointInterface position_interface_;

    joint_limits_interface::PositionJointSaturationInterface pj_sat_interface_;
    joint_limits_interface::PositionJointSoftLimitsInterface pj_limits_interface_;
    joint_limits_interface::EffortJointSaturationInterface ej_sat_interface_;
    joint_limits_interface::EffortJointSoftLimitsInterface ej_limits_interface_;

    GloveWrapper data_glove_;
    double glove_state_[11];
    ros::Publisher glove_raw_publisher_;

  protected:

  }; // SHHWSensorised class

  SHHWSensorised::SHHWSensorised(ros::NodeHandle nh) :
    nh_(nh)
  {
      glove_raw_publisher_ = nh_.advertise<std_msgs::Float64MultiArray>("raw_glove_state",1);


  }

  bool SHHWSensorised::start()
  {

    // construct a new lwr device (interface and state storage)
    this->device_.reset( new SHHWSensorised::SHRDevice() );

    nh_.param("device_id", device_id_, BROADCAST_ID);
    
    // TODO: use transmission configuration to get names directly from the URDF model
    if( ros::param::get("joints", this->device_->joint_names) )
    {
      if( !(this->device_->joint_names.size()==N_SYN) )
      {
        // that's fine for the sensorised case
        ROS_ERROR("This robot has 1 joint, you must specify 1 name only until more synergies are not included");
      } 
    }
    else
    {
      ROS_ERROR("No joints to be handled, ensure you load a yaml file naming the joint names this hardware interface refers to.");
      throw std::runtime_error("No joint name specification");
    }


    if( !(urdf_model_.initParam("robot_description")) )
    {
      ROS_ERROR("No URDF model in the robot_description parameter, this is required to define the joint limits.");
      throw std::runtime_error("No URDF model available");
    }

    // initialize and set to zero the state and command values
    this->device_->init(this->device_->joint_names.size());
    this->device_->reset();

    // general joint to store information
    boost::shared_ptr<const urdf::Joint> joint;

    // create joint handles given the list
    for(int i = 0; i < this->device_->joint_names.size(); ++i)
    {
      ROS_INFO_STREAM("Handling joint: " << this->device_->joint_names[i]);

      // get current joint configuration
      joint = urdf_model_.getJoint(this->device_->joint_names[i]);
      if(!joint.get())
      {
        ROS_ERROR_STREAM("The specified joint "<< this->device_->joint_names[i] << " can't be found in the URDF model. Check that you loaded an URDF model in the robot description, or that you spelled correctly the joint name.");
        throw std::runtime_error("Wrong joint name specification");
      }

      // joint state handle
      hardware_interface::JointStateHandle state_handle(this->device_->joint_names[i],
          &this->device_->joint_position[i],
          &this->device_->joint_velocity[i],
          &this->device_->joint_effort[i]);

      state_interface_.registerHandle(state_handle);


      // First joint is the synergy joint (which is also the only commanded joint)
      // effort command handle

   //   if( i == 0 ){

          hardware_interface::JointHandle joint_handle = hardware_interface::JointHandle(
            state_interface_.getHandle(this->device_->joint_names[i]),
            &this->device_->joint_position_command[i]);

          position_interface_.registerHandle(joint_handle);

          registerJointLimits(this->device_->joint_names[i],
                          joint_handle,
                          &urdf_model_,
                          &this->device_->joint_lower_limits[i],
                          &this->device_->joint_upper_limits[i],
                          &this->device_->joint_effort_limits[i]);

     // }
      

      
    }

    ROS_INFO("Register state and position interfaces");

    // register ros-controls interfaces
    this->registerInterface(&state_interface_);
    this->registerInterface(&position_interface_);

    // Finally, do the qb tools thing
    // get the port by id
    while(true)
    {
      if( port_selection(device_id_, port_) )
      {
        // open the port
        assert(open_port(port_));
        // and activate the hand
        commActivate(&comm_settings_t_, device_id_, 1);
        ROS_INFO("Done initialization !");
        return true;
      }
      else
      {
        ROS_WARN("Hand not found in all available ports, trying again...");
        // randomize the waiting to avoid conflict in files (probabilistically speaking)
        sleep( 4*((double) rand() / (RAND_MAX)) );
      }
    }



  }

  bool SHHWSensorised::read(ros::Time time, ros::Duration period)
  {
      // update the hand synergy joints
      // read from hand
      static short int inputs[2];
      commGetMeasurements(&comm_settings_t_, device_id_, inputs);

      static short int currents[2];
      commGetCurrents(&comm_settings_t_, device_id_, currents);

      // fill the state variables


      data_glove_.read(glove_state_);

      float mean_state = 0.0;

      std_msgs::Float64MultiArray raw_readings;
      raw_readings.data.resize(11);
      
      for(int i = 0; i < 11; i++){
        raw_readings.data[i] = glove_state_[i];
      }
    //   for(int i = 0; i < 5; i++){
    //     mean_state += glove_state_[i];
        
    //   }
    //   mean_state /= 5.0;
    

      std::map<int, std::pair<int,int> > joint_idx_map;
      joint_idx_map[0] = std::make_pair(1,4);
      joint_idx_map[1] = std::make_pair(4,8);
      joint_idx_map[2] = std::make_pair(8,12);
      joint_idx_map[3] = std::make_pair(12,16);
      joint_idx_map[4] = std::make_pair(16,20);

      this->device_->joint_position[0] = inputs[0]/17000.0; //synergy
      this->device_->joint_position_prev[0] = this->device_->joint_position[0]; //synergy
      for(int j = 0; j < 5; j++){

          std::pair<int,int> ids = joint_idx_map[j];

          for(int k = ids.first; k < ids.second; k++){

              // Not setting abd joints, except for thumb
              if(this->device_->joint_names[k].find("abd") == std::string::npos || this->device_->joint_names[k].find("thumb") != std::string::npos){
                  this->device_->joint_position_prev[k] = this->device_->joint_position[k];
                  this->device_->joint_position[k] = glove_state_[j]; //mean_state;
                  this->device_->joint_effort[k] = currents[0]*1.0;
              }
          }
      }

      for (int j = 0; j < this->device_->joint_names.size(); j++)
      {

          this->device_->joint_velocity[j] = filters::exponentialSmoothing((this->device_->joint_position[j]-this->device_->joint_position_prev[j])/period.toSec(), this->device_->joint_velocity[j], 0.2);
         
      }

      this->glove_raw_publisher_.publish(raw_readings);

      return true;
  }

  void SHHWSensorised::write(ros::Time time, ros::Duration period)
    {
        static int warning = 0;

        // enforce limits
        pj_sat_interface_.enforceLimits(period);
        pj_limits_interface_.enforceLimits(period);

        // write to the hand
        short int pos;
        pos = (short int)(17000.0*this->device_->joint_position_command[0]);
        set_input(pos);

        return;
    }

    void SHHWSensorised::stop()
    {
      usleep(2000000);
      // Deactivate motors
      commActivate(&comm_settings_t_, device_id_, 0);
      closeRS485(&comm_settings_t_);
    }

    void SHHWSensorised::set_mode()
    {
        // the hand does not have something like this, anyway it is left here to keep the template
        return;
    }

  // Register the limits of the joint specified by joint_name and\ joint_handle. The limits are
  // retrieved from the urdf_model.
  // Return the joint's type, lower position limit, upper position limit, and effort limit.
  void SHHWSensorised::registerJointLimits(const std::string& joint_name,
                           const hardware_interface::JointHandle& joint_handle,
                           const urdf::Model *const urdf_model,
                           double *const lower_limit, double *const upper_limit, 
                           double *const effort_limit)
  {
    *lower_limit = -std::numeric_limits<double>::max();
    *upper_limit = std::numeric_limits<double>::max();
    *effort_limit = std::numeric_limits<double>::max();

    joint_limits_interface::JointLimits limits;
    bool has_limits = false;
    joint_limits_interface::SoftJointLimits soft_limits;
    bool has_soft_limits = false;

    if (urdf_model != NULL)
    {
      const boost::shared_ptr<const urdf::Joint> urdf_joint = urdf_model->getJoint(joint_name);
      if (urdf_joint != NULL)
      {
        // Get limits from the URDF file.
        if (joint_limits_interface::getJointLimits(urdf_joint, limits))
          has_limits = true;
        if (joint_limits_interface::getSoftJointLimits(urdf_joint, soft_limits))
          has_soft_limits = true;
      }
    }

    if (!has_limits)
      return;

    if (limits.has_position_limits)
    {
      *lower_limit = limits.min_position;
      *upper_limit = limits.max_position;
    }
    if (limits.has_effort_limits)
      *effort_limit = limits.max_effort;

    if (has_soft_limits)
    {
      const joint_limits_interface::EffortJointSoftLimitsHandle limits_handle(joint_handle, limits, soft_limits);
      ej_limits_interface_.registerHandle(limits_handle);
    }
    else
    {
      const joint_limits_interface::EffortJointSaturationHandle sat_handle(joint_handle, limits);
      ej_sat_interface_.registerHandle(sat_handle);
    }
  }

  // port selection by id
  int SHHWSensorised::port_selection(const int id, char* my_port)
  {
    int num_ports = 0;
    char ports[10][255];

    num_ports = RS485listPorts(ports);

    ROS_DEBUG_STREAM("Search id in " << num_ports << " serial ports available...");

    if(num_ports)
    {
      for(int i = 0; i < num_ports; i++)
      {
        ROS_DEBUG_STREAM("Checking port: " << ports[i]);

        int aux_int;
        comm_settings comm_settings_t;
        char list_of_devices[255];

        openRS485(&comm_settings_t, ports[i]);

        if(comm_settings_t.file_handle == INVALID_HANDLE_VALUE)
        {
          ROS_DEBUG_STREAM("Couldn't connect to the serial port. Continue with the next available.");
          continue;
        }

        aux_int = RS485ListDevices(&comm_settings_t, list_of_devices);

        ROS_DEBUG_STREAM( "Number of devices: " << aux_int );

        if(aux_int > 1 || aux_int <= 0)
        {
          ROS_WARN_STREAM("The current port has " << aux_int << " devices connected, but it must be only one... that is not a SoftHand");
        }
        else
        {
          ROS_DEBUG_STREAM("List of devices:");
          for(int d = 0; d < aux_int; ++d)
          {
            ROS_DEBUG_STREAM( static_cast<int>(list_of_devices[d]) );
            ROS_DEBUG_STREAM( "searching id" << id );
            if( static_cast<int>(list_of_devices[d]) == id )
            {
              ROS_DEBUG_STREAM("Hand found at port: " << ports[i] << " !");
              strcpy(my_port, ports[i]);
              closeRS485(&comm_settings_t);
              sleep(1);
              return 1;
            }
            sleep(1);
          }
        }
        closeRS485(&comm_settings_t);
      }
      return 0;
    }
    else
    {
        ROS_ERROR("No serial port available.");
        return 0;
    }
  }

  int SHHWSensorised::open_port(char* port) 
  {
    ROS_DEBUG_STREAM("Opening serial port: " << port << " for hand_id: " << device_id_);
    fflush(stdout);

    openRS485(&comm_settings_t_, port);

    if(comm_settings_t_.file_handle == INVALID_HANDLE_VALUE)
    {
        ROS_ERROR("Couldn't connect to the selected serial port.");
        return 0;
    }
    usleep(500000);
    printf("Done.\n");
    return 1;
  }

  void SHHWSensorised::set_input(short int pos)
  {
    static short int inputs[2];

    inputs[0] = pos;
    inputs[1] = 0;

    commSetInputs(&comm_settings_t_, device_id_, inputs);
    return;
  }
}

int main( int argc, char** argv )
{
  // initialize ROS
  ros::init(argc, argv, "lwr_hw_interface", ros::init_options::NoSigintHandler);

  // ros spinner
  ros::AsyncSpinner spinner(1);
  spinner.start();

  // custom signal handlers
  signal(SIGTERM, quitRequested);
  signal(SIGINT, quitRequested);
  signal(SIGHUP, quitRequested);

  // construct the lwr
  ros::NodeHandle sh_nh("");
  soft_hand_hw::SHHWSensorised sh_robot(sh_nh);

  // configuration routines
  sh_robot.start();

  // timer variables
  struct timespec ts = {0, 0};
  ros::Time last(ts.tv_sec, ts.tv_nsec), now(ts.tv_sec, ts.tv_nsec);
  ros::Duration period(1.0);

  //the controller manager
  controller_manager::ControllerManager manager(&sh_robot, sh_nh);

  // run as fast as possible
  while( !g_quit ) 
  {
    // get the time / period
    if (!clock_gettime(CLOCK_REALTIME, &ts)) 
    {
      now.sec = ts.tv_sec;
      now.nsec = ts.tv_nsec;
      period = now - last;
      last = now;
    } 
    else 
    {
      ROS_FATAL("Failed to poll realtime clock!");
      break;
    } 

    // read the state from the soft hand
    if(!sh_robot.read(now, period))
    {
      g_quit = true;
      break;
    }

    // update the controllers
    manager.update(now, period);

    // write the command to the lwr
    sh_robot.write(now, period);
  }

  std::cerr <<" Stopping spinner..." << std::endl;
  spinner.stop();

  std::cerr << "Stopping soft hand..." << std::endl;
  sh_robot.stop();

  std::cerr << "Done!" << std::endl;

  return 0;
}
