from __future__ import annotations

import argparse
import importlib
import json
import os
import sys
import time
from ctypes import POINTER, byref, c_ubyte, cast, memset, sizeof
from pathlib import Path
from typing import Any


DEFAULT_SDK_ROOT = "/opt/MVS"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Stream Hikrobot MVS camera frames as MJPEG on stdout.")
    parser.add_argument("--sdk-root", default=DEFAULT_SDK_ROOT)
    parser.add_argument("--list", action="store_true", help="List MVS devices and exit.")
    parser.add_argument("--json", action="store_true", help="Emit JSON for --list.")
    parser.add_argument("--device-index", type=int, default=0)
    parser.add_argument("--serial", default="")
    parser.add_argument("--model", default="")
    parser.add_argument("--width", type=int, default=1280)
    parser.add_argument("--height", type=int, default=1024)
    parser.add_argument("--fps", type=int, default=15)
    parser.add_argument("--frames", type=int, default=0, help="Number of frames to emit, 0 means forever.")
    parser.add_argument("--timeout-ms", type=int, default=1000)
    parser.add_argument("--jpeg-quality", type=int, default=80)
    args = parser.parse_args(argv)

    try:
        mvs = _load_mvs(args.sdk_root)
        if args.list:
            devices = list_devices(mvs)
            if args.json:
                print(json.dumps({"device_count": len(devices), "devices": devices}, sort_keys=True))
            else:
                for index, device in enumerate(devices):
                    print(f"{index}: {device.get('model', '-')} serial={device.get('serial', '-')}")
            return 0
        stream_mjpeg(mvs, args)
        return 0
    except Exception as exc:
        print(f"mvs-camera-bridge: {exc}", file=sys.stderr)
        return 1


def _load_mvs(sdk_root: str) -> Any:
    import_path = Path(sdk_root) / "Samples" / "64" / "Python" / "MvImport"
    if not import_path.is_dir():
        raise RuntimeError(f"MVS Python import path not found: {import_path}")
    _prepare_mvs_environment(Path(sdk_root))
    import_path_text = str(import_path)
    if import_path_text not in sys.path:
        sys.path.insert(0, import_path_text)
    return importlib.import_module("MvCameraControl_class")


def _prepare_mvs_environment(sdk_root: Path) -> None:
    lib64 = sdk_root / "lib" / "64"
    lib32 = sdk_root / "lib" / "32"
    os.environ.setdefault("MVCAM_SDK_PATH", str(sdk_root))
    os.environ.setdefault("MVCAM_COMMON_RUNENV", str(sdk_root / "lib"))
    os.environ.setdefault("MVCAM_GENICAM_CLPROTOCOL", str(sdk_root / "lib" / "CLProtocol"))
    current_library_path = os.environ.get("LD_LIBRARY_PATH", "")
    required_paths = [str(lib64), str(lib32)]
    library_parts = [part for part in current_library_path.split(":") if part]
    for path in reversed(required_paths):
        if path not in library_parts:
            library_parts.insert(0, path)
    os.environ["LD_LIBRARY_PATH"] = ":".join(library_parts)


def list_devices(mvs: Any) -> list[dict[str, Any]]:
    mvs.MvCamera.MV_CC_Initialize()
    try:
        device_list = mvs.MV_CC_DEVICE_INFO_LIST()
        layer_type = _layer_type(mvs)
        ret = mvs.MvCamera.MV_CC_EnumDevices(layer_type, device_list)
        _check(ret, "enum devices")
        return [_device_info_to_dict(mvs, device_list, index) for index in range(device_list.nDeviceNum)]
    finally:
        mvs.MvCamera.MV_CC_Finalize()


def stream_mjpeg(mvs: Any, args: argparse.Namespace) -> None:
    if args.frames < 0:
        raise ValueError("--frames must be >= 0")
    if not 50 <= args.jpeg_quality <= 99:
        raise ValueError("--jpeg-quality must be between 50 and 99")

    mvs.MvCamera.MV_CC_Initialize()
    cam = mvs.MvCamera()
    opened = False
    grabbing = False
    try:
        device_list = mvs.MV_CC_DEVICE_INFO_LIST()
        ret = mvs.MvCamera.MV_CC_EnumDevices(_layer_type(mvs), device_list)
        _check(ret, "enum devices")
        if device_list.nDeviceNum == 0:
            raise RuntimeError("no MVS camera found")

        index = _select_device_index(mvs, device_list, args)
        device_info = cast(device_list.pDeviceInfo[index], POINTER(mvs.MV_CC_DEVICE_INFO)).contents
        _check(cam.MV_CC_CreateHandle(device_info), "create handle")
        _check(cam.MV_CC_OpenDevice(mvs.MV_ACCESS_Exclusive, 0), "open device")
        opened = True
        _configure_camera(mvs, cam, device_info, args)
        _check(cam.MV_CC_StartGrabbing(), "start grabbing")
        grabbing = True
        emitted = 0
        while args.frames == 0 or emitted < args.frames:
            frame = _capture_jpeg(mvs, cam, timeout_ms=args.timeout_ms, jpeg_quality=args.jpeg_quality)
            sys.stdout.buffer.write(frame)
            sys.stdout.buffer.flush()
            emitted += 1
    finally:
        if grabbing:
            cam.MV_CC_StopGrabbing()
        if opened:
            cam.MV_CC_CloseDevice()
        try:
            cam.MV_CC_DestroyHandle()
        finally:
            mvs.MvCamera.MV_CC_Finalize()


def _layer_type(mvs: Any) -> int:
    return (
        mvs.MV_GIGE_DEVICE
        | mvs.MV_USB_DEVICE
        | mvs.MV_GENTL_CAMERALINK_DEVICE
        | mvs.MV_GENTL_CXP_DEVICE
        | mvs.MV_GENTL_XOF_DEVICE
    )


def _select_device_index(mvs: Any, device_list: Any, args: argparse.Namespace) -> int:
    if args.serial or args.model:
        for index in range(device_list.nDeviceNum):
            info = _device_info_to_dict(mvs, device_list, index)
            if args.serial and info.get("serial") == args.serial:
                return index
            if args.model and info.get("model") == args.model:
                return index
        wanted = f"serial={args.serial}" if args.serial else f"model={args.model}"
        raise RuntimeError(f"MVS camera not found: {wanted}")
    if args.device_index < 0 or args.device_index >= device_list.nDeviceNum:
        raise RuntimeError(f"MVS camera index {args.device_index} out of range; found {device_list.nDeviceNum}")
    return args.device_index


def _configure_camera(mvs: Any, cam: Any, device_info: Any, args: argparse.Namespace) -> None:
    if device_info.nTLayerType in (mvs.MV_GIGE_DEVICE, mvs.MV_GENTL_GIGE_DEVICE):
        packet_size = cam.MV_CC_GetOptimalPacketSize()
        if int(packet_size) > 0:
            cam.MV_CC_SetIntValue("GevSCPSPacketSize", packet_size)
    cam.MV_CC_SetEnumValue("TriggerMode", mvs.MV_TRIGGER_MODE_OFF)
    if args.width > 0:
        cam.MV_CC_SetIntValue("Width", int(args.width))
    if args.height > 0:
        cam.MV_CC_SetIntValue("Height", int(args.height))
    if args.fps > 0:
        cam.MV_CC_SetBoolValue("AcquisitionFrameRateEnable", True)
        cam.MV_CC_SetFloatValue("AcquisitionFrameRate", float(args.fps))


def _capture_jpeg(mvs: Any, cam: Any, *, timeout_ms: int, jpeg_quality: int) -> bytes:
    frame = mvs.MV_FRAME_OUT()
    memset(byref(frame), 0, sizeof(frame))
    ret = cam.MV_CC_GetImageBuffer(frame, timeout_ms)
    _check(ret, "get image buffer")
    try:
        frame_info = frame.stFrameInfo
        output_size = max(int(frame_info.nWidth) * int(frame_info.nHeight) * 4 + 4096, int(frame_info.nFrameLen) * 4, 1024 * 1024)
        output = (c_ubyte * output_size)()
        save_param = mvs.MV_SAVE_IMAGE_PARAM_EX3()
        memset(byref(save_param), 0, sizeof(save_param))
        save_param.pData = cast(frame.pBufAddr, POINTER(c_ubyte))
        save_param.nDataLen = frame_info.nFrameLen
        save_param.enPixelType = frame_info.enPixelType
        save_param.nWidth = frame_info.nWidth
        save_param.nHeight = frame_info.nHeight
        save_param.pImageBuffer = output
        save_param.nBufferSize = output_size
        save_param.enImageType = mvs.MV_Image_Jpeg
        save_param.nJpgQuality = jpeg_quality
        save_param.iMethodValue = 1
        _check(cam.MV_CC_SaveImageEx3(save_param), "save image")
        return bytes(output[: save_param.nImageLen])
    finally:
        cam.MV_CC_FreeImageBuffer(frame)


def _device_info_to_dict(mvs: Any, device_list: Any, index: int) -> dict[str, Any]:
    info = cast(device_list.pDeviceInfo[index], POINTER(mvs.MV_CC_DEVICE_INFO)).contents
    layer = info.nTLayerType
    if layer in (mvs.MV_GIGE_DEVICE, mvs.MV_GENTL_GIGE_DEVICE):
        detail = info.SpecialInfo.stGigEInfo
        current_ip = detail.nCurrentIp
        return {
            "index": index,
            "type": "gige",
            "model": _decode_c_chars(detail.chModelName),
            "serial": _decode_c_chars(detail.chSerialNumber),
            "ip": ".".join(str((current_ip >> shift) & 0xFF) for shift in (24, 16, 8, 0)),
        }
    if layer == mvs.MV_USB_DEVICE:
        detail = info.SpecialInfo.stUsb3VInfo
        return {
            "index": index,
            "type": "u3v",
            "model": _decode_c_chars(detail.chModelName),
            "serial": _decode_c_chars(detail.chSerialNumber),
        }
    return {"index": index, "type": str(layer), "model": "", "serial": ""}


def _decode_c_chars(values: Any) -> str:
    raw = bytes(int(value) & 0xFF for value in values if int(value) != 0)
    return raw.decode("utf-8", errors="replace")


def _check(ret: int, action: str) -> None:
    if ret != 0:
        raise RuntimeError(f"{action} failed ret[0x{int(ret):x}]")


if __name__ == "__main__":
    raise SystemExit(main())
