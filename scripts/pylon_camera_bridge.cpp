#include <pylon/PylonIncludes.h>
#include <GenApi/GenApi.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include <unistd.h>

using namespace Pylon;
using namespace GenApi;

struct Args {
    bool list = false;
    bool json = false;
    int device_index = 0;
    std::string serial;
    std::string model;
    int width = 1280;
    int height = 1024;
    int fps = 15;
    int frames = 0;
    int timeout_ms = 1000;
};

[[noreturn]] void finish(int code)
{
    std::cout.flush();
    std::cerr.flush();
    std::_Exit(code);
}

std::string require_value(int& index, int argc, char** argv, const std::string& option)
{
    if (index + 1 >= argc) {
        throw std::runtime_error(option + " requires a value");
    }
    ++index;
    return argv[index];
}

int parse_int(const std::string& value, const std::string& option)
{
    try {
        size_t consumed = 0;
        int parsed = std::stoi(value, &consumed);
        if (consumed != value.size()) {
            throw std::invalid_argument("trailing input");
        }
        return parsed;
    } catch (const std::exception&) {
        throw std::runtime_error(option + " must be an integer: " + value);
    }
}

Args parse_args(int argc, char** argv)
{
    Args args;
    for (int i = 1; i < argc; ++i) {
        std::string option = argv[i];
        if (option == "--list") {
            args.list = true;
        } else if (option == "--json") {
            args.json = true;
        } else if (option == "--device-index") {
            args.device_index = parse_int(require_value(i, argc, argv, option), option);
        } else if (option == "--serial") {
            args.serial = require_value(i, argc, argv, option);
        } else if (option == "--model") {
            args.model = require_value(i, argc, argv, option);
        } else if (option == "--width") {
            args.width = parse_int(require_value(i, argc, argv, option), option);
        } else if (option == "--height") {
            args.height = parse_int(require_value(i, argc, argv, option), option);
        } else if (option == "--fps") {
            args.fps = parse_int(require_value(i, argc, argv, option), option);
        } else if (option == "--frames") {
            args.frames = parse_int(require_value(i, argc, argv, option), option);
        } else if (option == "--timeout-ms") {
            args.timeout_ms = parse_int(require_value(i, argc, argv, option), option);
        } else if (option == "--jpeg-quality") {
            (void)require_value(i, argc, argv, option);
        } else if (option == "-h" || option == "--help") {
            std::cout
                << "Usage: pylon-camera-bridge [--list --json] [--device-index N|--serial S|--model M]\n"
                << "                           [--width W] [--height H] [--fps FPS] [--frames N]\n";
            finish(0);
        } else {
            throw std::runtime_error("unknown option: " + option);
        }
    }
    if (args.frames < 0) {
        throw std::runtime_error("--frames must be >= 0");
    }
    return args;
}

std::string to_string(const String_t& value)
{
    return std::string(value.c_str());
}

std::string json_escape(const std::string& value)
{
    std::string escaped;
    for (char ch : value) {
        switch (ch) {
            case '\\':
                escaped += "\\\\";
                break;
            case '"':
                escaped += "\\\"";
                break;
            case '\n':
                escaped += "\\n";
                break;
            case '\r':
                escaped += "\\r";
                break;
            case '\t':
                escaped += "\\t";
                break;
            default:
                escaped += ch;
                break;
        }
    }
    return escaped;
}

DeviceInfoList_t enumerate_devices()
{
    DeviceInfoList_t devices;
    CTlFactory::GetInstance().EnumerateDevices(devices);
    return devices;
}

int selected_device_index(const DeviceInfoList_t& devices, const Args& args)
{
    if (devices.empty()) {
        throw std::runtime_error("no Basler pylon camera found");
    }
    if (!args.serial.empty() || !args.model.empty()) {
        for (size_t i = 0; i < devices.size(); ++i) {
            const CDeviceInfo& info = devices[i];
            if (!args.serial.empty() && to_string(info.GetSerialNumber()) == args.serial) {
                return static_cast<int>(i);
            }
            if (!args.model.empty() && to_string(info.GetModelName()) == args.model) {
                return static_cast<int>(i);
            }
        }
        throw std::runtime_error(args.serial.empty() ? "Basler camera model not found: " + args.model
                                                     : "Basler camera serial not found: " + args.serial);
    }
    if (args.device_index < 0 || static_cast<size_t>(args.device_index) >= devices.size()) {
        throw std::runtime_error("Basler camera index out of range");
    }
    return args.device_index;
}

void list_devices(const Args& args)
{
    DeviceInfoList_t devices = enumerate_devices();
    if (args.json) {
        std::cout << "{\"device_count\":" << devices.size() << ",\"devices\":[";
        for (size_t i = 0; i < devices.size(); ++i) {
            const CDeviceInfo& info = devices[i];
            if (i > 0) {
                std::cout << ",";
            }
            std::cout << "{\"index\":" << i
                      << ",\"model\":\"" << json_escape(to_string(info.GetModelName()))
                      << "\",\"serial\":\"" << json_escape(to_string(info.GetSerialNumber()))
                      << "\",\"type\":\"" << json_escape(to_string(info.GetDeviceClass()))
                      << "\"}";
        }
        std::cout << "]}" << std::endl;
    } else {
        for (size_t i = 0; i < devices.size(); ++i) {
            const CDeviceInfo& info = devices[i];
            std::cout << i << ": " << info.GetModelName() << " serial=" << info.GetSerialNumber() << std::endl;
        }
    }
    finish(0);
}

int64_t clamp_integer_node(CIntegerPtr node, int value)
{
    int64_t min_value = node->GetMin();
    int64_t max_value = node->GetMax();
    int64_t inc = std::max<int64_t>(1, node->GetInc());
    int64_t wanted = std::clamp<int64_t>(value, min_value, max_value);
    return min_value + ((wanted - min_value) / inc) * inc;
}

void set_integer_node(INodeMap& node_map, const char* name, int value)
{
    try {
        CIntegerPtr node(node_map.GetNode(name));
        if (IsWritable(node)) {
            node->SetValue(clamp_integer_node(node, value));
        }
    } catch (const GenericException& exc) {
        std::cerr << "pylon-camera-bridge: could not set " << name << ": " << exc.GetDescription() << std::endl;
    }
}

void set_fps(INodeMap& node_map, int fps)
{
    if (fps <= 0) {
        return;
    }
    try {
        CBooleanPtr enable(node_map.GetNode("AcquisitionFrameRateEnable"));
        if (IsWritable(enable)) {
            enable->SetValue(true);
        }
    } catch (const GenericException&) {
    }
    try {
        CFloatPtr rate(node_map.GetNode("AcquisitionFrameRate"));
        if (IsWritable(rate)) {
            double wanted = std::clamp<double>(static_cast<double>(fps), rate->GetMin(), rate->GetMax());
            rate->SetValue(wanted);
        }
    } catch (const GenericException& exc) {
        std::cerr << "pylon-camera-bridge: could not set AcquisitionFrameRate: " << exc.GetDescription() << std::endl;
    }
}

void configure_camera(CInstantCamera& camera, const Args& args)
{
    INodeMap& node_map = camera.GetNodeMap();
    if (args.width > 0) {
        set_integer_node(node_map, "Width", args.width);
    }
    if (args.height > 0) {
        set_integer_node(node_map, "Height", args.height);
    }
    set_fps(node_map, args.fps);
}

std::vector<char> read_file(const std::string& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("could not read saved frame: " + path);
    }
    return std::vector<char>((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

void stream_mjpeg(const Args& args)
{
    DeviceInfoList_t devices = enumerate_devices();
    int index = selected_device_index(devices, args);
    CInstantCamera* camera = new CInstantCamera(CTlFactory::GetInstance().CreateDevice(devices[index]));
    camera->Open();
    configure_camera(*camera, args);
    camera->StartGrabbing(GrabStrategy_LatestImageOnly);

    std::string temp_path = "/tmp/mine-teleop-pylon-frame-" + std::to_string(static_cast<long long>(getpid())) + ".jpg";
    int emitted = 0;
    while (args.frames == 0 || emitted < args.frames) {
        CGrabResultPtr grab_result;
        camera->RetrieveResult(args.timeout_ms, grab_result, TimeoutHandling_ThrowException);
        if (!grab_result->GrabSucceeded()) {
            throw std::runtime_error("grab failed with error " + std::to_string(grab_result->GetErrorCode()) + ": "
                                     + std::string(grab_result->GetErrorDescription().c_str()));
        }
        CImagePersistence::Save(ImageFileFormat_Jpeg, temp_path.c_str(), grab_result);
        std::vector<char> jpeg = read_file(temp_path);
        if (jpeg.empty()) {
            throw std::runtime_error("saved frame was empty");
        }
        std::cout.write(jpeg.data(), static_cast<std::streamsize>(jpeg.size()));
        std::cout.flush();
        ++emitted;
    }
    std::remove(temp_path.c_str());
    finish(0);
}

int main(int argc, char** argv)
{
    try {
        Args args = parse_args(argc, argv);
        PylonInitialize();
        if (args.list) {
            list_devices(args);
        }
        stream_mjpeg(args);
    } catch (const GenericException& exc) {
        std::cerr << "pylon-camera-bridge: " << exc.GetDescription() << std::endl;
        finish(1);
    } catch (const std::exception& exc) {
        std::cerr << "pylon-camera-bridge: " << exc.what() << std::endl;
        finish(1);
    }
}
