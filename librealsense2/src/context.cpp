// License: Apache 2.0. See LICENSE file in root directory.
// Copyright(c) 2015 Intel Corporation. All Rights Reserved.
#ifdef _MSC_VER
#if (_MSC_VER <= 1800) // constexpr is not supported in MSVC2013
#error( "Librealsense requires MSVC2015 or later to build. Compilation will be aborted" )
#endif
#endif

#include <array>

#include "ivcam/sr300.h"
#include "ds5/ds5-factory.h"
#include "ds5/ds5-timestamp.h"
#include "backend.h"
#include "mock/recorder.h"
#include <chrono>
#include <media/ros/ros_reader.h>
#include "types.h"
#include "context.h"

template<unsigned... Is> struct seq{};
template<unsigned N, unsigned... Is>
struct gen_seq : gen_seq<N-1, N-1, Is...>{};
template<unsigned... Is>
struct gen_seq<0, Is...> : seq<Is...>{};

template<unsigned N1, unsigned... I1, unsigned N2, unsigned... I2>
constexpr std::array<char const, N1+N2-1> concat(char const (&a1)[N1], char const (&a2)[N2], seq<I1...>, seq<I2...>){
  return {{ a1[I1]..., a2[I2]... }};
}

template<unsigned N1, unsigned N2>
constexpr std::array<char const, N1+N2-1> concat(char const (&a1)[N1], char const (&a2)[N2]){
  return concat(a1, a2, gen_seq<N1-1>{}, gen_seq<N2>{});
}

// The string is used to retrieve the version embedded into .so file on Linux
constexpr auto rs2_api_version = concat("VERSION: ",RS2_API_VERSION_STR);

namespace librealsense
{
    context::context(backend_type type,
                     const char* filename,
                     const char* section,
                     rs2_recording_mode mode)
        : _devices_changed_callback(nullptr , [](rs2_devices_changed_callback*){})
    {

        LOG_DEBUG("Librealsense " << std::string(std::begin(rs2_api_version),std::end(rs2_api_version)));

        switch(type)
        {
        case backend_type::standard:
            _backend = platform::create_backend();
            break;
        case backend_type::record:
            _backend = std::make_shared<platform::record_backend>(platform::create_backend(), filename, section, mode);
            break;
        case backend_type::playback:
            _backend = std::make_shared<platform::playback_backend>(filename, section);

            break;
        default: throw invalid_value_exception(to_string() << "Undefined backend type " << static_cast<int>(type));
        }

       _ts = _backend->create_time_service();

       _device_watcher = _backend->create_device_watcher();
    }

    class platform_camera : public device
    {
    public:
        platform_camera(const platform::backend& backend,const  platform::uvc_device_info uvc_info, std::shared_ptr<platform::time_service> ts)
        {
            auto uvc_dev = backend.create_uvc_device(uvc_info);
            auto color_ep = std::make_shared<uvc_sensor>("RGB Camera", uvc_dev, std::unique_ptr<ds5_timestamp_reader>(new ds5_timestamp_reader(ts)), ts, this);
            add_sensor(color_ep);

            register_info(RS2_CAMERA_INFO_NAME, "Platform Camera");
            std::string pid_str(to_string() << std::setfill('0') << std::setw(4) << std::hex << uvc_info.pid);
            std::transform(pid_str.begin(), pid_str.end(), pid_str.begin(), ::toupper);

            register_info(RS2_CAMERA_INFO_SERIAL_NUMBER, uvc_info.unique_id);
            register_info(RS2_CAMERA_INFO_LOCATION, uvc_info.device_path);
            register_info(RS2_CAMERA_INFO_PRODUCT_ID, pid_str);

            color_ep->register_pixel_format(pf_yuy2);
            color_ep->register_pixel_format(pf_yuyv);

            color_ep->register_pu(RS2_OPTION_BACKLIGHT_COMPENSATION);
            color_ep->register_pu(RS2_OPTION_BRIGHTNESS);
            color_ep->register_pu(RS2_OPTION_CONTRAST);
            color_ep->register_pu(RS2_OPTION_EXPOSURE);
            color_ep->register_pu(RS2_OPTION_GAMMA);
            color_ep->register_pu(RS2_OPTION_HUE);
            color_ep->register_pu(RS2_OPTION_SATURATION);
            color_ep->register_pu(RS2_OPTION_SHARPNESS);
            color_ep->register_pu(RS2_OPTION_WHITE_BALANCE);
            color_ep->register_pu(RS2_OPTION_ENABLE_AUTO_EXPOSURE);
            color_ep->register_pu(RS2_OPTION_ENABLE_AUTO_WHITE_BALANCE);

            color_ep->set_pose(lazy<pose>([](){pose p = {{ { 1,0,0 },{ 0,1,0 },{ 0,0,1 } },{ 0,0,0 }}; return p; }));
        }

        virtual rs2_intrinsics get_intrinsics(unsigned int subdevice, const stream_profile& profile) const
        {
            return rs2_intrinsics {};
        }
    };

    std::shared_ptr<device_interface> platform_camera_info::create(const platform::backend& backend) const
    {
        return std::make_shared<platform_camera>(backend, _uvc, backend.create_time_service());
    }

    context::~context()
    {
        _device_watcher->stop(); //ensure that the device watcher will stop before the _devices_changed_callback will be deleted
    }

    std::vector<std::shared_ptr<device_info>> context::query_devices() const
    {

        platform::backend_device_group devices(_backend->query_uvc_devices(), _backend->query_usb_devices(), _backend->query_hid_devices());

        return create_devices(devices, _playback_devices);
    }

    std::vector<std::shared_ptr<device_info>> context::create_devices(platform::backend_device_group devices,
                                                                      const std::map<std::string, std::shared_ptr<device_info>>& playback_devices) const
    {
        std::vector<std::shared_ptr<device_info>> list;

        auto ds5_devices = ds5_info::pick_ds5_devices(_backend, devices);
        std::copy(begin(ds5_devices), end(ds5_devices), std::back_inserter(list));

        auto sr300_devices = sr300_info::pick_sr300_devices(_backend, devices.uvc_devices, devices.usb_devices);
        std::copy(begin(sr300_devices), end(sr300_devices), std::back_inserter(list));

        auto recovery_devices = recovery_info::pick_recovery_devices(_backend, devices.usb_devices);
        std::copy(begin(recovery_devices), end(recovery_devices), std::back_inserter(list));

        auto uvc_devices = platform_camera_info::pick_uvc_devices(_backend, devices.uvc_devices);
        std::copy(begin(uvc_devices), end(uvc_devices), std::back_inserter(list));

        for (auto&& item : playback_devices)
        {
            list.push_back(item.second);
        }

        return list;
    }


    void context::on_device_changed(platform::backend_device_group old,
                                    platform::backend_device_group curr,
                                    const std::map<std::string, std::shared_ptr<device_info>>& old_playback_devices,
                                    const std::map<std::string, std::shared_ptr<device_info>>& new_playback_devices)
    {
        auto old_list = create_devices(old, old_playback_devices);
        auto new_list = create_devices(curr, new_playback_devices);

        if (librealsense::list_changed<std::shared_ptr<device_info>>(old_list, new_list, [](std::shared_ptr<device_info> first, std::shared_ptr<device_info> second) {return *first == *second; }))
        {

            std::vector<rs2_device_info> rs2_devices_info_added;
            std::vector<rs2_device_info> rs2_devices_info_removed;

            auto devices_info_removed = subtract_sets(old_list, new_list);

            for (auto i=0; i<devices_info_removed.size(); i++)
            {
                rs2_devices_info_removed.push_back({ shared_from_this(), devices_info_removed[i] });
                LOG_DEBUG("\nDevice disconnected:\n\n" << std::string(devices_info_removed[i]->get_device_data()));
            }

            auto devices_info_added = subtract_sets(new_list, old_list);
            for (auto i = 0; i<devices_info_added.size(); i++)
            {
                rs2_devices_info_added.push_back({ shared_from_this(), devices_info_added[i] });
                LOG_DEBUG("\nDevice connected:\n\n" << std::string(devices_info_added[i]->get_device_data()));
            }

            _devices_changed_callback->on_devices_changed(  new rs2_device_list({ shared_from_this(), rs2_devices_info_removed }),
                                                            new rs2_device_list({ shared_from_this(), rs2_devices_info_added }));
        }
    }

    double context::get_time()
    {
        return _ts->get_time();
    }

    void context::set_devices_changed_callback(devices_changed_callback_ptr callback)
    {
        _device_watcher->stop();

        _devices_changed_callback = std::move(callback);
        _device_watcher->start([this](platform::backend_device_group old, platform::backend_device_group curr)
        {
            on_device_changed(old, curr, _playback_devices, _playback_devices);
        });
    }

    std::shared_ptr<device_interface> context::add_device(const std::string& file)
    {
        if(_playback_devices.find(file) != _playback_devices.end())
        {
            //Already exists
            throw librealsense::invalid_value_exception(to_string() << "File \"" << file << "\" already loaded to context");
        }
        auto playack_dev = std::make_shared<playback_device>(std::make_shared<ros_reader>(file));
        auto dinfo = std::make_shared<playback_device_info>(playack_dev);
        auto prev_playback_devices =_playback_devices;
        _playback_devices[file] = dinfo;
        on_device_changed({},{}, prev_playback_devices, _playback_devices);
        return playack_dev;
    }

    void context::remove_device(const std::string& file)
    {
        auto it = _playback_devices.find(file);
        if(it == _playback_devices.end())
        {
            //Not found
            return;
        }
        auto prev_playback_devices =_playback_devices;
        _playback_devices.erase(it);
        on_device_changed({},{}, prev_playback_devices, _playback_devices);
    }
}