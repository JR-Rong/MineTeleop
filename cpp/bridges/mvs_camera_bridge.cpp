#include <MvCameraControl.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

struct Arguments {
  bool list{false};
  bool json{false};
  int device_index{0};
  std::string serial;
  std::string model;
  int width{1280};
  int height{720};
  int fps{30};
  int frames{0};
  int timeout_ms{2000};
  int jpeg_quality{80};
};

std::string required_value(int& index, int argc, char** argv, std::string_view option) {
  if (index + 1 >= argc) throw std::invalid_argument(std::string(option) + " requires a value");
  return argv[++index];
}

int integer(std::string_view value, std::string_view option) {
  std::size_t consumed = 0;
  int result = 0;
  try {
    result = std::stoi(std::string(value), &consumed);
  } catch (const std::exception&) {
    throw std::invalid_argument(std::string(option) + " must be an integer");
  }
  if (consumed != value.size()) throw std::invalid_argument(std::string(option) + " must be an integer");
  return result;
}

Arguments parse_arguments(int argc, char** argv) {
  Arguments arguments;
  for (int index = 1; index < argc; ++index) {
    const std::string option(argv[index]);
    if (option == "--list") arguments.list = true;
    else if (option == "--json") arguments.json = true;
    else if (option == "--device-index") arguments.device_index = integer(required_value(index, argc, argv, option), option);
    else if (option == "--serial") arguments.serial = required_value(index, argc, argv, option);
    else if (option == "--model") arguments.model = required_value(index, argc, argv, option);
    else if (option == "--width") arguments.width = integer(required_value(index, argc, argv, option), option);
    else if (option == "--height") arguments.height = integer(required_value(index, argc, argv, option), option);
    else if (option == "--fps") arguments.fps = integer(required_value(index, argc, argv, option), option);
    else if (option == "--frames") arguments.frames = integer(required_value(index, argc, argv, option), option);
    else if (option == "--timeout-ms") arguments.timeout_ms = integer(required_value(index, argc, argv, option), option);
    else if (option == "--jpeg-quality") arguments.jpeg_quality = integer(required_value(index, argc, argv, option), option);
    else if (option == "--help" || option == "-h") {
      std::cout << "Usage: mine-teleop-mvs-camera [--list --json] [--device-index N|--serial S|--model M] "
                   "[--width W] [--height H] [--fps FPS] [--frames N] [--jpeg-quality 1..99]\n";
      std::exit(0);
    } else {
      throw std::invalid_argument("unknown option: " + option);
    }
  }
  if (arguments.device_index < 0 || arguments.width <= 0 || arguments.height <= 0 || arguments.fps <= 0 ||
      arguments.frames < 0 || arguments.timeout_ms <= 0 || arguments.jpeg_quality < 1 || arguments.jpeg_quality > 99) {
    throw std::invalid_argument("MVS camera numeric option is out of range");
  }
  return arguments;
}

void check(int result, std::string_view operation) {
  if (result != MV_OK) {
    throw std::runtime_error(std::string(operation) + " failed with MVS status 0x" + [&] {
      constexpr char digits[] = "0123456789abcdef";
      std::string value(8, '0');
      const auto raw = static_cast<std::uint32_t>(result);
      for (int index = 7; index >= 0; --index) value[static_cast<std::size_t>(index)] = digits[(raw >> ((7 - index) * 4)) & 0xF];
      std::reverse(value.begin(), value.end());
      return value;
    }());
  }
}

std::string c_string(const unsigned char* value, std::size_t capacity) {
  const auto length = strnlen(reinterpret_cast<const char*>(value), capacity);
  return std::string(reinterpret_cast<const char*>(value), length);
}

struct DeviceDescription {
  std::string model;
  std::string serial;
  std::string transport;
};

DeviceDescription describe(const MV_CC_DEVICE_INFO& info) {
  if (info.nTLayerType == MV_GIGE_DEVICE) {
    return {
        c_string(info.SpecialInfo.stGigEInfo.chModelName, sizeof(info.SpecialInfo.stGigEInfo.chModelName)),
        c_string(info.SpecialInfo.stGigEInfo.chSerialNumber, sizeof(info.SpecialInfo.stGigEInfo.chSerialNumber)),
        "gige",
    };
  }
  if (info.nTLayerType == MV_USB_DEVICE) {
    return {
        c_string(info.SpecialInfo.stUsb3VInfo.chModelName, sizeof(info.SpecialInfo.stUsb3VInfo.chModelName)),
        c_string(info.SpecialInfo.stUsb3VInfo.chSerialNumber, sizeof(info.SpecialInfo.stUsb3VInfo.chSerialNumber)),
        "usb3",
    };
  }
  return {"", "", "unknown"};
}

std::string json_escape(std::string_view value) {
  std::string result;
  for (const char character : value) {
    if (character == '\\' || character == '"') result.push_back('\\');
    if (character == '\n') result += "\\n";
    else if (character != '\r') result.push_back(character);
  }
  return result;
}

MV_CC_DEVICE_INFO_LIST enumerate() {
  MV_CC_DEVICE_INFO_LIST devices{};
  check(MV_CC_EnumDevices(MV_GIGE_DEVICE | MV_USB_DEVICE, &devices), "MV_CC_EnumDevices");
  return devices;
}

int selected_index(const MV_CC_DEVICE_INFO_LIST& devices, const Arguments& arguments) {
  if (devices.nDeviceNum == 0) throw std::runtime_error("no Hikrobot MVS camera found");
  if (!arguments.serial.empty() || !arguments.model.empty()) {
    for (unsigned int index = 0; index < devices.nDeviceNum; ++index) {
      if (devices.pDeviceInfo[index] == nullptr) continue;
      const auto info = describe(*devices.pDeviceInfo[index]);
      if ((!arguments.serial.empty() && info.serial == arguments.serial) ||
          (!arguments.model.empty() && info.model == arguments.model)) return static_cast<int>(index);
    }
    throw std::runtime_error(arguments.serial.empty() ? "MVS camera model not found" : "MVS camera serial not found");
  }
  if (arguments.device_index >= static_cast<int>(devices.nDeviceNum)) throw std::runtime_error("MVS camera index out of range");
  return arguments.device_index;
}

void list_devices(const MV_CC_DEVICE_INFO_LIST& devices, bool json) {
  if (json) std::cout << "{\"device_count\":" << devices.nDeviceNum << ",\"devices\":[";
  for (unsigned int index = 0; index < devices.nDeviceNum; ++index) {
    if (devices.pDeviceInfo[index] == nullptr) continue;
    const auto info = describe(*devices.pDeviceInfo[index]);
    if (json) {
      if (index > 0) std::cout << ',';
      std::cout << "{\"index\":" << index << ",\"model\":\"" << json_escape(info.model)
                << "\",\"serial\":\"" << json_escape(info.serial) << "\",\"type\":\"" << info.transport << "\"}";
    } else {
      std::cout << index << ": " << info.model << " serial=" << info.serial << " type=" << info.transport << '\n';
    }
  }
  if (json) std::cout << "]}\n";
}

class Camera {
 public:
  explicit Camera(MV_CC_DEVICE_INFO* info) {
    check(MV_CC_CreateHandle(&handle_, info), "MV_CC_CreateHandle");
    try {
      check(MV_CC_OpenDevice(handle_), "MV_CC_OpenDevice");
      opened_ = true;
    } catch (...) {
      MV_CC_DestroyHandle(handle_);
      handle_ = nullptr;
      throw;
    }
  }

  ~Camera() {
    if (grabbing_) MV_CC_StopGrabbing(handle_);
    if (opened_) MV_CC_CloseDevice(handle_);
    if (handle_ != nullptr) MV_CC_DestroyHandle(handle_);
  }

  void configure(const Arguments& arguments) {
    MV_CC_SetIntValueEx(handle_, "Width", static_cast<std::uint64_t>(arguments.width));
    MV_CC_SetIntValueEx(handle_, "Height", static_cast<std::uint64_t>(arguments.height));
    MV_CC_SetBoolValue(handle_, "AcquisitionFrameRateEnable", true);
    MV_CC_SetFloatValue(handle_, "AcquisitionFrameRate", static_cast<float>(arguments.fps));
    if (MV_CC_SetEnumValueByString(handle_, "PixelFormat", "BayerRG8") != MV_OK) {
      MV_CC_SetEnumValueByString(handle_, "PixelFormat", "Mono8");
    }
  }

  void start() {
    check(MV_CC_StartGrabbing(handle_), "MV_CC_StartGrabbing");
    grabbing_ = true;
  }

  std::vector<unsigned char> jpeg(int timeout_ms, int quality) {
    MV_FRAME_OUT frame{};
    check(MV_CC_GetImageBuffer(handle_, &frame, timeout_ms), "MV_CC_GetImageBuffer");
    struct Guard {
      void* handle;
      MV_FRAME_OUT* frame;
      ~Guard() { MV_CC_FreeImageBuffer(handle, frame); }
    } guard{handle_, &frame};
    std::vector<unsigned char> output(
        std::max<std::size_t>(1024 * 1024, static_cast<std::size_t>(frame.stFrameInfo.nWidth) * frame.stFrameInfo.nHeight * 3 + 4096));
    MV_SAVE_IMAGE_PARAM_EX save{};
    save.enImageType = MV_Image_Jpeg;
    save.enPixelType = frame.stFrameInfo.enPixelType;
    save.nWidth = frame.stFrameInfo.nWidth;
    save.nHeight = frame.stFrameInfo.nHeight;
    save.pData = frame.pBufAddr;
    save.nDataLen = frame.stFrameInfo.nFrameLen;
    save.pImageBuffer = output.data();
    save.nBufferSize = static_cast<unsigned int>(output.size());
    save.nJpgQuality = quality;
    check(MV_CC_SaveImageEx2(handle_, &save), "MV_CC_SaveImageEx2");
    output.resize(save.nImageLen);
    return output;
  }

 private:
  void* handle_{nullptr};
  bool opened_{false};
  bool grabbing_{false};
};

}  // namespace

int main(int argc, char** argv) {
  try {
    const auto arguments = parse_arguments(argc, argv);
    auto devices = enumerate();
    if (arguments.list) {
      list_devices(devices, arguments.json);
      return 0;
    }
    const auto index = selected_index(devices, arguments);
    Camera camera(devices.pDeviceInfo[index]);
    camera.configure(arguments);
    camera.start();
    int emitted = 0;
    while (arguments.frames == 0 || emitted < arguments.frames) {
      const auto jpeg = camera.jpeg(arguments.timeout_ms, arguments.jpeg_quality);
      std::cout.write(reinterpret_cast<const char*>(jpeg.data()), static_cast<std::streamsize>(jpeg.size()));
      std::cout.flush();
      ++emitted;
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "mine-teleop-mvs-camera: " << error.what() << '\n';
    return 2;
  }
}
