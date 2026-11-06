# PR #26 修复与瞬态链路保持实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在 PR #26 分支上完成审查修复（阶段 1–4），并实现 `docs/64-transient-link-hold-design.md`
定义的瞬态链路保持，让控制链路 1000 ms、视频 1200 ms 以内的临时尖峰不再触发安全停车。

**Architecture:** 车端 `VehicleControlService` 持有一个纯逻辑的 `TransientHoldPolicy`，统一判断进入保持、
自动恢复和升级为现有制动等待；驾驶端只在车端声明能力后发送签名能力字段，并把只是往返偏高的 ACK
从"立即停车"改为"持续达到容忍上限才停车"；页面把视频异常分为瞬态和故障上报。审查修复按阶段先行，
其中阶段 2 删除 mock 双路实验，为瞬态保持精简控制链路。

**Tech Stack:** C++20、CMake/CTest、GStreamer 1.20+（webrtcbin、libnice）、nlohmann::json、yaml-cpp、
浏览器端 JavaScript（Node 22 跑测试）、Python 3（测试与分析脚本）、Docker（Ubuntu 22.04 本地验证）。

## Global Constraints

- 设计依据：`docs/64-transient-link-hold-design.md`（含规划阶段修订）；与本计划冲突时以设计文档为准，并先停下来确认。
- 不放宽"新鲜"的定义：命令单向年龄上限 200 ms（`max_command_gap_ms`）、视频新鲜度门限 500 ms 都不变。
- 保持期间：牵引恒为 0；转向、挡位保持在进入保持前最后一条新鲜命令的值；制动取驾驶员制动（车端归一化后）和 `hold_brake` 的较大值。
- `transient_hold` 默认值与范围：`max_hold_ms` 700（100–1500）、`hold_brake` 0.15（0–0.3）、`resume_fresh_commands` 2（1–5）、`budget_window_ms` 60000（10000–600000）、`max_holds_per_window` 6（1–100）、`max_hold_time_per_window_ms` 3000（100 至 `budget_window_ms`）。
- 启动校验：`degraded_timeout_ms + max_hold_ms + 100 ≤ control_timeout_ms`；`bridge_apply_timeout_ms` 取 20–60000，不大于 `control_timeout_ms`，不小于 `field_safety.speed_feedback_timeout_ms`，缺省等于 `control_timeout_ms`。
- 代码默认：YAML 没有 `transient_hold` 块时视为关闭；`control_timeout_ms` 代码默认 800。仓库现场配置 `vehicle-agent.three-machine.field.yaml`、`vehicle-agent.ccg2-8m.yaml` 显式开启，`control_timeout_ms: 1200`、`bridge_apply_timeout_ms: 800`。
- 能力字符串 `"transient-hold-v1"`；命令签名字段 `transient_hold`、`critical_video_transient`；offer 字段 `transient_hold_capability`、`transient_hold: {control_tolerance_ms, video_tolerance_ms, max_hold_ms}`。驾驶端只接受 `control_tolerance_ms` 为 400–2000 的整数，`video_tolerance_ms` 为 1–4500 的整数。
- 故障类原因（急停、输入租约、校时、`control_link_ready=false`、视频故障、相机故障、非法样本、故障类制动修订号、失去控制权、底盘/CAN 故障）行为不变。
- 制动在一次停车过程中只增不减；急停锁存时机仍按分级值判断。
- 云端代码不改。Windows 原生 mock 客户端不发送 `transient-hold-v1`。
- 每个任务完成后提交一次；提交信息用英文祈使句标题（沿用仓库风格），正文结尾加 `Co-Authored-By: Claude <noreply@anthropic.com>`。未经用户同意不推送、不修改 PR 说明、不做任何对外发布。
- 只能在 Windows CI 上编译的代码（`cpp/apps/windows_native_viewer.cpp`、`packaging/windows/**`），修改后需要推送分支触发 Windows 工作流验证；推送前必须先征得用户同意。
- C++ 测试沿用仓库写法：`main()` + 抛异常的 `require`/`expect` + 结尾打印 `PASS ...`，用 `add_executable` + `add_test` 注册。JS 测试沿用各文件自带的 `test(名称, 函数)` 和 `assert.strict`。Python 测试用 `unittest`。

---

## Task 0：本地验证环境与基线

**Files:** 无（不提交）。以下脚本放在仓库之外（`/tmp/mt-tools/`），因为本机的 Docker 引擎是 Colima，**不共享任何宿主机目录**（bind mount 得到空目录），所以源码要用 tar 流写入命名卷。

- [ ] **Step 1：构建与 CI 同款的 Ubuntu 22.04 测试镜像**（只需一次）。完整测试构建要求 Node（`CMakeLists.txt:667` 的 `find_program(... REQUIRED)`），Ubuntu 22.04 自带的 Node 12 跑不了 `.mjs` 和 `?.` 语法的测试，所以从官方站点下载 Node 22 并校验 SHA-256：

```bash
docker build -t mine-teleop-native-ci:22.04 - <<'EOF'
FROM ubuntu:22.04
ARG DEBIAN_FRONTEND=noninteractive
ARG NODE_VERSION=22.20.0
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential cmake pkg-config iproute2 coturn openssl ca-certificates python3 curl xz-utils \
      libunwind-dev libcurl4-openssl-dev libssl-dev libyaml-cpp-dev nlohmann-json3-dev libjpeg-dev \
      libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev libgstreamer-plugins-bad1.0-dev libnice-dev \
      gstreamer1.0-plugins-base gstreamer1.0-plugins-good gstreamer1.0-plugins-bad \
      gstreamer1.0-plugins-ugly gstreamer1.0-libav gstreamer1.0-nice \
    && rm -rf /var/lib/apt/lists/*
RUN set -eux; \
    case "$(dpkg --print-architecture)" in amd64) node_arch=x64 ;; arm64) node_arch=arm64 ;; *) exit 1 ;; esac; \
    base="https://nodejs.org/dist/v${NODE_VERSION}"; file="node-v${NODE_VERSION}-linux-${node_arch}.tar.xz"; \
    mkdir /tmp/node-download; cd /tmp/node-download; \
    curl -fsSLO "$base/$file"; curl -fsSLO "$base/SHASUMS256.txt"; \
    grep " $file\$" SHASUMS256.txt | sha256sum -c -; \
    tar -xJf "$file" -C /usr/local --strip-components=1; \
    cd /; rm -rf /tmp/node-download; node --version
EOF
```

Expected：构建成功；`docker run --rm mine-teleop-native-ci:22.04 node --version` 输出 `v22.20.0`。

- [ ] **Step 2：创建两个辅助脚本**

`/tmp/mt-tools/native-sync.sh`：把源码目录镜像进命名卷 `mine-teleop-native-src`。只发送内容有变化的文件，并用 `tar -m` 解包，使这些文件的 mtime 是容器内的"现在"，因此无论宿主机与虚拟机时钟差多少，make 都恰好重编译这些文件。必须带 `--no-xattrs` 和 `COPYFILE_DISABLE=1`，否则 macOS 的 tar 会夹带 `._*` 文件进入卷：

```bash
#!/usr/bin/env bash
set -euo pipefail
export LC_ALL=C
src="$1"; vol=mine-teleop-native-src
state="/tmp/mt-logs/manifest.$vol"; new="$(mktemp)"
cd "$src"
docker volume inspect "$vol" >/dev/null 2>&1 || rm -f "$state"
touch "$state"
if [ -e .git ]; then
  git ls-files -z -c -o --exclude-standard | while IFS= read -r -d '' f; do [ -f "$f" ] && printf '%s\0' "$f"; done
else
  find . -type f -print | sed 's#^\./##' | tr '\n' '\0'
fi | xargs -0 shasum -a 256 | sort > "$new"
changed="$(comm -13 <(sort "$state") "$new" | cut -c67-)"
removed="$(comm -23 <(cut -c67- "$state" | sort) <(cut -c67- "$new" | sort))"
docker volume create "$vol" >/dev/null
if [ -n "$changed" ]; then
  printf '%s\n' "$changed" | tr '\n' '\0' | COPYFILE_DISABLE=1 tar --no-xattrs --null -T - -cf - \
    | docker run --rm -i -v "$vol":/src ubuntu:22.04 tar -xmf - -C /src
fi
if [ -n "$removed" ]; then
  printf '%s\n' "$removed" | docker run --rm -i -v "$vol":/src ubuntu:22.04 \
    sh -c 'cd /src; while IFS= read -r f; do rm -f -- "$f"; done'
fi
mv "$new" "$state"
echo "sync: $(printf '%s' "$changed" | grep -c . || true) changed/new, $(printf '%s' "$removed" | grep -c . || true) removed"
```

`/tmp/mt-tools/native-check.sh <源码目录> [ctest 参数...]`：同步后，在无网络容器里按 CI 的参数构建全部原生测试目标并运行 CTest（`NET_ADMIN` 只为 netem 测试）：

```bash
#!/usr/bin/env bash
set -euo pipefail
src="$1"; shift
/tmp/mt-tools/native-sync.sh "$src"
exec docker run --rm --network none --cap-add NET_ADMIN \
  -v mine-teleop-native-src:/src -v mine-teleop-native-build:/build -w /src mine-teleop-native-ci:22.04 bash -euc '
    cmake -S /src -B /build -DCMAKE_BUILD_TYPE=Debug \
      -DMINE_TELEOP_BUILD_TESTS=ON -DMINE_TELEOP_BUILD_PORTABLE_CONTROL_TESTS=ON \
      -DMINE_TELEOP_BUILD_VEHICLE_RUNTIME=ON -DMINE_TELEOP_BUILD_BENCH_PROBES=ON \
      -DMINE_TELEOP_BUILD_VENDOR_CAMERA_BRIDGES=OFF -DMINE_TELEOP_BUILD_ARAVIS_CAMERA_BRIDGE=OFF \
      -DMINE_TELEOP_FETCH_MISSING_DEPS=OFF
    cmake --build /build --parallel 8
    ctest --test-dir /build --output-on-failure --timeout 180 "$@"' _ "$@"
```

后文所有"Task 0 Step 2 的 Docker 命令"都指 `/tmp/mt-tools/native-check.sh "$PWD"`（要只跑部分测试，在后面加 `-R <正则>`）。注意：基线运行期间不要再同步源码，也不要同时启动第二个使用这两个卷的容器。

宿主机上的纯 Python/JS 回归：

```bash
python3 scripts/test/run_offline_regressions.py
```

- [ ] **Step 3：在干净的当前 HEAD 上跑一次基线**。工作区若已有未提交修改，先用 `git archive HEAD | tar -x -C /tmp/mt-baseline` 导出干净副本，再对该目录运行 `native-check.sh`，不要在带修改的工作区上跑基线。

Expected：记录通过、失败、跳过的测试名单作为基线。后续任务不得新增失败，也不得把基线失败或跳过的项算作通过。

---

# 第一部分　阶段 1：生产路径修复（A1–A5、A7）

## Task 1：libnice 选中对观测改为尽力而为（A1）

**Files:**
- Modify: `cpp/src/webrtc_media.cpp`（`observe_ice_agent()` 约 4040–4047 行；调用处约 4220–4235 行）
- Test: `cpp/tests/ice_vehicle_callbacks.test.py`

**Interfaces:**
- Produces: `bool observe_ice_agent()`，拿不到 libnice agent 时返回 false，不再抛异常；新增诊断事件 `vehicle_ice_selected_pair_unavailable`。

- [ ] **Step 1：写失败的源码契约测试**

在 `cpp/tests/ice_vehicle_callbacks.test.py` 的 `main()` 末尾、打印成功信息之前加入：

```python
    observe = section(source, "  bool observe_ice_agent() {", "\n  }\n")
    if "throw" in observe:
        raise AssertionError("selected-pair observation must never abort the media pipeline")
    if "vehicle_ice_selected_pair_unavailable" not in observe:
        raise AssertionError("missing diagnostic when libnice selected-pair observation is unavailable")
    call_site = section(source, "      configure_ice_servers();", "    } catch (const std::exception& error) {")
    if "observe_ice_agent();" not in call_site:
        raise AssertionError("ICE agent observation call site moved unexpectedly")
```

其中 `source` 是该脚本 `main()` 中已有的 `source = args.source.read_text()`，`section()` 是脚本已有的辅助函数。

- [ ] **Step 2：运行测试，确认失败**

Run: Task 0 Step 2 的 Docker 命令，末尾的 `ctest` 加 `-R ice-vehicle-callback-mock`（该脚本需要 CMake 传入的 `--output`、`--compiler`、`--json-include` 参数，不在宿主机直接运行）。
Expected: FAIL，提示找不到 `  bool observe_ice_agent() {`。

- [ ] **Step 3：修改实现**

把 `void observe_ice_agent()` 替换为：

```cpp
  // Diagnostic only: the selected pair is never required to stream media.
  // Upstream documents the libnice "agent" property as unsafe to rely on, so
  // its absence must not fail the pipeline.
  bool observe_ice_agent() {
    GObject* ice = nullptr;
    g_object_get(webrtc, "ice-agent", &ice, nullptr);
    if (ice) {
      if (g_object_class_find_property(G_OBJECT_GET_CLASS(ice), "agent")) g_object_get(ice, "agent", &ice_agent, nullptr);
      g_object_unref(ice);
    }
    if (!ice_agent) {
      std::string negotiation;
      { std::lock_guard lock(media_signal_state->mutex); negotiation = media_signal_state->negotiation_id; }
      emit_diagnostic("vehicle_ice_selected_pair_unavailable", "ice_selected_pair_unavailable", "ice_connectivity", "",
          "Media continues; selected-pair diagnostics are unavailable on this GStreamer build.", true,
          {{"media_negotiation_id", negotiation}});
      return false;
    }
    auto* state = new std::shared_ptr<MediaSignalState>(media_signal_state);
    g_signal_connect_data(ice_agent, "new-selected-pair-full", G_CALLBACK(on_selected_ice_pair), state,
        +[](gpointer data, GClosure*) { delete static_cast<std::shared_ptr<MediaSignalState>*>(data); }, static_cast<GConnectFlags>(0));
    return true;
  }
```

调用处保持 `observe_ice_agent();` 一行不变（返回值有意忽略，失败已经发出诊断）。

- [ ] **Step 4：运行测试，确认通过；再跑 Task 0 Step 2 的原生全量验证**

Expected：`mine-teleop-ice-vehicle-callback-mock-tests` PASS；CTest 全部通过（`mine-teleop-vehicle-ice-fallback-runtime-tests` 等不回归）。

- [ ] **Step 5：提交**

```bash
git add cpp/src/webrtc_media.cpp cpp/tests/ice_vehicle_callbacks.test.py
git commit -m "Keep vehicle media running without libnice pair observation" -m "Co-Authored-By: Claude <noreply@anthropic.com>"
```

## Task 2：keep-alive 回归测试注册进 CTest（A3）

**Files:**
- Modify: `CMakeLists.txt`（`if(MINE_TELEOP_BUILD_TESTS OR MINE_TELEOP_BUILD_PORTABLE_CONTROL_TESTS)` 块内、`find_package(Python3 ...)` 之后）

- [ ] **Step 1：注册测试**

```cmake
  if(TARGET mine-teleop-control)
    add_test(NAME mine-teleop-control-http-keepalive-tests
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/cpp/tests/control_http_keepalive.test.py"
        $<TARGET_FILE:mine-teleop-control>)
    set_tests_properties(mine-teleop-control-http-keepalive-tests PROPERTIES
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 60)
  endif()
```

- [ ] **Step 2：运行原生全量验证**

Run: Task 0 Step 2 的 Docker 命令，然后单独确认：`docker run ... ctest --test-dir /build -R keepalive --output-on-failure`
Expected：`mine-teleop-control-http-keepalive-tests` 出现在测试列表中并 PASS。

- [ ] **Step 3：提交**

```bash
git add CMakeLists.txt
git commit -m "Run control HTTP keep-alive regression on every platform" -m "Co-Authored-By: Claude <noreply@anthropic.com>"
```

## Task 3：ICE 列表刷新失败时降级而不是中止（A4、A5）

**Files:**
- Modify: `cpp/include/mine_teleop/detail/ice_route_policy.hpp`（新增刷新辅助函数）
- Modify: `cpp/src/webrtc_media.cpp`（约 5455 行 `ice_configuration = signaling.ice_servers();`）
- Modify: `cpp/web/control_logic.js`（新增并导出 `refreshIceServersOrCached`）
- Modify: `cpp/src/server.cpp`（内嵌页面 `startFromOffer` 中 `if(refreshIce){...}`，约 1807–1812 行）
- Test: `cpp/tests/ice_route_policy_tests.cpp`、`cpp/tests/control_logic.test.cjs`

**Interfaces:**
- Produces（C++）：`template <typename Fetch, typename Stop> std::pair<nlohmann::json, bool> refresh_ice_configuration(Fetch fetch, Stop check_stop, const nlohmann::json& previous, int attempts = 3)`；返回 `{配置, 是否刷新成功}`。
- Produces（JS）：`async refreshIceServersOrCached(fetchServers, cached, routeMode)` → `{servers, refreshed, error?}`；`routeMode === 'relay'` 时刷新失败直接抛出。

- [ ] **Step 1：写失败的 C++ 测试**

在 `cpp/tests/ice_route_policy_tests.cpp` 的 `main()` 中、打印成功信息之前加入：

```cpp
    int calls = 0;
    const nlohmann::json cached = {{"ice_servers", {{{"urls", {"stun:cached.example"}}}}}};
    const auto failed = refresh_ice_configuration([&]() -> nlohmann::json { ++calls; throw std::runtime_error("signaling down"); },
        [] {}, cached, 3);
    require(!failed.second && failed.first == cached && calls == 3, "failed refresh did not keep the cached ICE list after bounded retries");
    calls = 0;
    const auto refreshed = refresh_ice_configuration([&]() -> nlohmann::json {
          if (++calls < 2) throw std::runtime_error("transient"); return {{"ice_servers", nlohmann::json::array()}}; },
        [] {}, cached, 3);
    require(refreshed.second && refreshed.first.at("ice_servers").empty() && calls == 2, "retry did not adopt the refreshed list");
    bool stopped = false;
    try { static_cast<void>(refresh_ice_configuration([]() -> nlohmann::json { return {}; },
        [] { throw std::runtime_error("stop requested"); }, cached, 3)); } catch (const std::runtime_error&) { stopped = true; }
    require(stopped, "stop request was swallowed by the refresh helper");
```

文件顶部补 `#include <nlohmann/json.hpp>` 和 `#include <utility>`。

- [ ] **Step 2：写失败的 JS 测试**

在 `cpp/tests/control_logic.test.cjs` 中、汇总输出之前加入：

```js
test('renegotiation keeps cached ICE servers when the refresh fails outside relay mode', async () => {
  const cached = [{urls: ['stun:cached.example']}];
  const ok = await logic.refreshIceServersOrCached(async () => [{urls: ['turn:new.example']}], cached, 'all');
  assert.deepEqual(ok, {servers: [{urls: ['turn:new.example']}], refreshed: true});
  const fallback = await logic.refreshIceServersOrCached(async () => { throw new Error('offline'); }, cached, 'direct');
  assert.equal(fallback.refreshed, false);
  assert.deepEqual(fallback.servers, cached);
  await assert.rejects(() => logic.refreshIceServersOrCached(async () => { throw new Error('offline'); }, cached, 'relay'), /offline/);
});
test('control page renegotiation uses the cached-ICE fallback helper', () => {
  assert.match(controlPageSource, /controlLogic\.refreshIceServersOrCached\(fetchIceServers,iceServers,routeMode\)/);
});
```

- [ ] **Step 3：运行两个测试，确认失败**

Run: `node cpp/tests/control_logic.test.cjs`（宿主机）；C++ 测试走 Task 0 Step 2。
Expected：JS 报 `logic.refreshIceServersOrCached is not a function`；C++ 编译失败（函数未定义）。

- [ ] **Step 4：实现 C++ 辅助函数**

在 `ice_route_policy.hpp` 顶部补 `#include <nlohmann/json.hpp>`、`#include <exception>`、`#include <utility>`，命名空间内追加：

```cpp
// Fallback attempts refresh short-lived TURN credentials. A transient
// signaling failure keeps the previous configuration (its STUN entries still
// work); a stop request always propagates.
template <typename Fetch, typename Stop>
inline std::pair<nlohmann::json, bool> refresh_ice_configuration(Fetch fetch, Stop check_stop,
    const nlohmann::json& previous, int attempts = 3) {
  for (int attempt = 0; attempt < attempts; ++attempt) {
    check_stop();
    try {
      return {fetch(), true};
    } catch (const std::exception&) {
    }
  }
  check_stop();
  return {previous, false};
}
```

- [ ] **Step 5：在车端回退路径使用它**

把 `webrtc_media.cpp` 中回退分支的：

```cpp
        check_stop();
        ice_configuration = signaling.ice_servers();
        check_stop();
```

替换为：

```cpp
        // check_stop is a local [&] lambda in the media loop, so pass it directly.
        auto [refreshed_ice, ice_refreshed] = detail::refresh_ice_configuration(
            [this] { return signaling.ice_servers(); }, check_stop, ice_configuration);
        if (!ice_refreshed)
          emit_diagnostic("vehicle_ice_refresh_fallback", "ice_refresh_failed", "ice_configuration", "",
              "Retrying with the previous ICE list; TURN credentials may be expired.", true);
        ice_configuration = std::move(refreshed_ice);
```

- [ ] **Step 6：实现 JS 辅助函数并在页面使用**

在 `control_logic.js` 中 `iceServersForMode` 之后加入，并加进导出对象（与 `iceServersForMode` 并列）：

```js
  // Renegotiation refreshes short-lived TURN credentials. A failed refresh keeps
  // the cached list for direct/all attempts; a relay-only attempt cannot proceed.
  async function refreshIceServersOrCached(fetchServers, cached, routeMode) {
    try {
      return {servers: await fetchServers(), refreshed: true};
    } catch (error) {
      if (routeMode === 'relay') throw error;
      return {servers: Array.isArray(cached) ? cached : [], refreshed: false,
        error: String(error && error.message || error)};
    }
  }
```

把 `server.cpp` 页面中的：

```js
  if(refreshIce){
    let refreshed;
    try{refreshed=await fetchIceServers()}catch(error){if(!offerCurrent())return;throw error}
    if(!offerCurrent())return;
    iceServers=refreshed;attemptIceServers=controlLogic.iceServersForMode(iceServers,routeMode);
  }
```

替换为：

```js
  if(refreshIce){
    let refreshed;
    try{refreshed=await controlLogic.refreshIceServersOrCached(fetchIceServers,iceServers,routeMode)}catch(error){if(!offerCurrent())return;throw error}
    if(!offerCurrent())return;
    if(!refreshed.refreshed)clientLog('webrtc_ice_refresh_fallback',{mode:routeMode,error:refreshed.error});
    iceServers=refreshed.servers;attemptIceServers=controlLogic.iceServersForMode(iceServers,routeMode);
  }
```

- [ ] **Step 7：运行测试，确认通过**

Run: `node cpp/tests/control_logic.test.cjs` 和 Task 0 Step 2。
Expected：新增的 JS 用例 `[PASS]`；`mine-teleop-ice-route-policy-tests` PASS。

- [ ] **Step 8：提交**

```bash
git add cpp/include/mine_teleop/detail/ice_route_policy.hpp cpp/src/webrtc_media.cpp cpp/web/control_logic.js cpp/src/server.cpp cpp/tests/ice_route_policy_tests.cpp cpp/tests/control_logic.test.cjs
git commit -m "Fall back to cached ICE servers when a refresh fails" -m "Co-Authored-By: Claude <noreply@anthropic.com>"
```

## Task 4：DC 试探预算重置的回归测试（A7）——已被现有测试覆盖，跳过

执行时发现 `cpp/tests/control_transport_tests.cpp` 已有 `confirmed_profile_permits_only_one_new_bounded_dc_budget()`，
它完整覆盖该行为：耗尽 3 次预算；未确认 profile、运动命令被接受、过期的 WSS 接受都不重置预算；
一次新鲜的中性 WSS 接受恰好授予一个新的有界预算；第二轮预算同样有上限。重复加测试没有价值，本任务不写代码、不提交。
审查时认为"缺测试"是错误的；A7 剩下的工作只是在 PR 说明里把它列为生产行为变化（Task 27）。

## Task 5：`ice.*` 拒绝启动的升级说明（A2）

**Files:**
- Modify: `docs/DEPLOY.md`（"## 7. 升级和回滚"一节）

- [ ] **Step 1：追加升级条目**

在"## 7. 升级和回滚"末尾加入：

```markdown
### 升级到包含 PR #26 的版本

- 车端 YAML 中的 `ice.stun_servers` / `ice.turn_servers` 从未被读取，现在会导致车端拒绝启动，
  错误信息为 `vehicle ice.stun_servers/turn_servers are unsupported`。升级前删除这两个键；
  STUN/TURN 地址改由信令服务 `--stun-urls` / `--turn-urls` 配置。
- 使用仓库内打包配置部署的车辆不受影响。自维护配置请先用
  `./bin/mine-teleop-run config-check --config <你的配置>` 检查，看到上述错误即按本条处理。
```

- [ ] **Step 2：提交**

```bash
git add docs/DEPLOY.md
git commit -m "Document the vehicle ICE configuration upgrade step" -m "Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

# 第二部分　阶段 2：精简 mock 双路实验（删除镜像与紧凑格式；A6、B4、B6、B7）

本部分完成后，执行下面的检查必须没有任何输出（文档中的历史描述在 Task 11 处理）：

```bash
grep -rnE "mock_dual|MockDual|mock_wss_dc|MockWssDc|mock_compact|MockCompact|NativeMirrorIngressAcks|mock_media_status_v1|mock_dc_backpressure|mock_skip_dc|send_mock_compact|wss_dc_mirror" cpp CMakeLists.txt
```

## Task 6：删除紧凑 CBOR 控制与遥测格式

**Files:**
- Delete: `cpp/include/mine_teleop/detail/mock_compact_control.hpp`、`cpp/include/mine_teleop/detail/mock_compact_telemetry.hpp`
- Modify: `cpp/src/webrtc_media.cpp`、`cpp/include/mine_teleop/detail/native_control_channel.hpp`、`cpp/apps/windows_native_viewer.cpp`
- Test: `cpp/tests/control_transport_tests.cpp`、`cpp/tests/native_control_channel_tests.cpp`、`cpp/tests/ice_vehicle_callbacks.test.py`、`cpp/tests/ice_gstreamer_init.test.py`

- [ ] **Step 0：先搬走仍被使用的谓词。** `mock_compact_control.hpp` 里还定义了 `isolated_mock_control_allowed(...)`，它在 Task 7 之前仍被 `webrtc_media.cpp` 的 `mock_dual_control_enabled()` 使用。删除头文件之前，把这一个函数原样移到 `control_transport.hpp`（`kMockWssDcMirrorCapability` 附近、`mock_wss_dc_mirror_allowed` 之前），这样每个提交都能编译。Task 7 删除 `mock_dual_control_enabled()` 时一并删除它。
- [ ] **Step 1：删除车端紧凑格式代码**（`webrtc_media.cpp`）
  - 删除 `#include "mine_teleop/detail/mock_compact_control.hpp"` 和 `#include "mine_teleop/detail/mock_compact_telemetry.hpp"`。
  - 删除成员函数 `mock_compact_control_enabled()` 和静态回调 `on_control_message_data(...)`，以及 `g_signal_connect(channel, "on-message-data", G_CALLBACK(on_control_message_data), this);` 这一行。
  - offer 负载中删除 `{"mock_compact_control", ...}` 和 `{"mock_compact_dual_control", ...}` 两项。
  - 遥测发布函数中删除 `compact_telemetry_env` / `telemetry_now` / `compact_telemetry` 相关的整段（从 `const auto* compact_telemetry_env = std::getenv("MINE_TELEOP_MOCK_COMPACT_TELEMETRY");` 到 `last_mock_full_telemetry_ms = telemetry_now; }` 为止），并删除成员 `std::int64_t last_mock_full_telemetry_ms{0};`。
- [ ] **Step 2：删除驾驶端/viewer 的紧凑发送**
  - `native_control_channel.hpp`：删除 `send_mock_compact(...)`；把 `send_impl(const std::string& payload, bool binary)` 收敛为只发字符串：删除 `binary` 参数和 `gst_webrtc_data_channel_send_data_full` 分支，`send()` 直接调用 `send_impl(payload)`。
  - `windows_native_viewer.cpp`：删除 `#include "mine_teleop/detail/mock_compact_control.hpp"`；`run_mock_control` 删除 `compact_control`、`compact_dual` 两个参数及其传参；courier 的 send 回调改为 `const bool queued = channel->send(bytes);`，删除 `compact` 变量，trace 中的 `wire_format` 固定为 `"json"`、`wire_bytes` 为 `bytes.size()`；offer 处理中删除 `compact_control`、`compact_dual` 的计算。
- [ ] **Step 3：删除相关测试**
  - `control_transport_tests.cpp`：删除 `compact_mock_wire_preserves_the_signed_command()` 及其在 `main()` 中的调用，删除对 `mock_compact_control.hpp` / `mock_compact_telemetry.hpp` 的 include 和所有 `mock_compact_*` 用例。
  - `native_control_channel_tests.cpp`：删除 `send_mock_compact` 相关断言。
  - `ice_vehicle_callbacks.test.py`、`ice_gstreamer_init.test.py`：删除 fixture 中对 `mock_compact_control` / `mock_compact_dual_control` 字段的期望。
- [ ] **Step 4：删除头文件**

```bash
git rm cpp/include/mine_teleop/detail/mock_compact_control.hpp cpp/include/mine_teleop/detail/mock_compact_telemetry.hpp
```

- [ ] **Step 5：验证**

Run: `grep -rn "mock_compact\|MockCompact\|send_mock_compact\|MOCK_COMPACT" cpp CMakeLists.txt` → 无输出；再跑 Task 0 Step 2。
Expected：CTest 全部通过。

- [ ] **Step 6：提交**

```bash
git add -A cpp
git commit -m "Remove the experimental compact mock control wire" -m "Co-Authored-By: Claude <noreply@anthropic.com>"
```

## Task 7：删除 DC/WSS 双路镜像

**Files:**
- Modify: `cpp/include/mine_teleop/server.hpp`、`cpp/src/server.cpp`、`cpp/include/mine_teleop/detail/control_transport.hpp`、`cpp/include/mine_teleop/detail/control_transport_sender.hpp`、`cpp/include/mine_teleop/detail/native_control_acknowledgement_window.hpp`、`cpp/src/webrtc_media.cpp`、`cpp/include/mine_teleop/detail/native_control_publication.hpp`、`cpp/include/mine_teleop/detail/native_control_courier.hpp`、`cpp/apps/windows_native_viewer.cpp`、`CMakeLists.txt`
- Delete: `cpp/include/mine_teleop/detail/mock_dc_backpressure.hpp`
- Test: `cpp/tests/control_transport_tests.cpp`、`cpp/tests/control_transport_integration_tests.cpp`、`cpp/tests/feedback_vehicle_method_tests.cpp`、`cpp/tests/native_control_publication_tests.cpp`、`cpp/tests/control_tests.cpp`、`cpp/tests/ice_vehicle_callbacks.test.py`

**Interfaces:**
- Produces：`ControlTransportFence::admit(command, stamp_json, received_path, current_media_attempt, now_utc_ms, lease_expires_at_ms, max_command_gap_ms, cloud_authority_live, verified_stop_only = false)`（恢复为 9 个参数）；`NativeControlPublication(NativeCourierScope scope)`、`NativeControlCourier(NativeCourierScope scope)`（删除第二个参数）；集成夹具构造函数变为 `Fixture(bool direct = false, bool feedback = false, bool product = false, std::string mode = "p2p_preferred", bool defer_owner = false)`。

- [ ] **Step 1：协议层**
  - `control_transport.hpp`：删除 `kMockDualControlCapability`、`kMockWssDcMirrorCapability`、`mock_wss_dc_mirror_allowed()`；`admit()` 删除最后两个参数和 `mirrored` / `dc_mirror` 两个局部变量，路径判断恢复为：

```cpp
    if (stamp.path != received_path ||
        (stamp.path == ControlPath::DataChannel && stamp.media_attempt != current_media_attempt))
      return {false, false, "transport_path_or_attempt_mismatch"};
```

  - `control_transport_sender.hpp`：删除 `mock_dual_sender_allowed()` 和 `owns_wss_dc_mirror()`（保留 `mock_idle_reprobe_eligible`、`observe_mock_idle_reprobe` 和 profile 相关逻辑）。
  - `native_control_acknowledgement_window.hpp`：删除 `class NativeMirrorIngressAcks`。
  - `native_control_publication.hpp`：构造函数删除 `allow_wss_dc_mirror` 参数和成员；`take()` 删除 `mirror` 分支，条件恢复为 `if (transport.value("protocol", "") != kControlTransportCapability || !direct || ...)`。
  - `native_control_courier.hpp`：构造函数删除 `allow_wss_dc_mirror` 参数，`publication_(scope_)`。
- [ ] **Step 2：驾驶端**（`server.hpp` / `server.cpp`）
  - `DriverConfig` 删除 `bool mock_dual_control{false};` 及其注释。
  - 删除成员 `control_mirror_ingress_acks_`、`mock_dual_last_sent_ms_`、`mock_dual_last_vehicle_path_`、`mock_dual_last_vehicle_seq_`、`mock_dual_control_vehicle_capable_`、`mock_wss_dc_mirror_vehicle_capable_`，以及 `connect_control_signaling_websocket`、`close_control_signaling_websocket`、`reset_native_control_state` 中对它们的重置。
  - `consume_vehicle_control_result_locked`：删除 `if (config_.mock_dual_control && latency_sample && ...)` 块，恢复为 `static_cast<void>(control_vehicle_result_latency_.note_result(seq, intent, now, accepted, false, direct_return));`。
  - `control_transport_bridge`：删除 `wss_dc_mirror_capable`、`wss_dc_mirror` 和返回值中的 `"wss_dc_mirror_enabled"`；发布条件恢复为 `control_transport_sender_.owns_dc_publication(control_transport_latest_dc_)`。
  - `drain_native_control_acknowledgements_locked`：删除 `if (control_mirror_ingress_acks_.consume(acknowledged_seq)) {...}` 整块。
  - `send_native_control_sample`：删除 `vehicle_mock_dual_capable` / `vehicle_mock_wss_dc_capable` 的声明与快照、`mock_dual` / `mock_wss_dc` 两个布尔量及写入负载的 `mock_dual_control` / `mock_wss_dc_mirror` / `mock_wss_dc_attempt` 字段、DC 发布后的 WSS 镜像发送块，以及 WSS 发送后的 DC 镜像发布块。
  - `status()`：删除 `mock_dual_last_vehicle_path`、`mock_dual_control`、`vehicle_received_path` 三个字段。
  - `poll_signaling`：删除对 `mock_dual_control` / `mock_wss_dc_mirror` 能力的解析。
- [ ] **Step 3：车端**（`webrtc_media.cpp`）
  - 删除 `#include "mine_teleop/detail/mock_dc_backpressure.hpp"`、成员函数 `mock_dual_control_enabled()`，offer 中的 `mock_dual_control` / `mock_wss_dc_mirror` 两项。
  - `handle_control_message`：删除 `mock_dual_dc`、`mock_wss_dc`、`mock_dual`；`command_received_path` 的条件恢复为 `if (config.runtime.experimental_direct_authority_max_ms > 0)`；`via_dc` 删除 `|| mock_dual`；`if (!native_feedback || mock_dual)` 恢复为 `if (!native_feedback)`；删除 `dc_backpressure_skipped` 整段（`dc_send_invoked` 改回 `const bool`），trace 中删除 `dc_backpressure_skipped`；`admit(...)` 调用删除最后两个实参；删除 `if (mock_dual && decision.reason == "transport_sequence_replayed") {...}` 块。
  - `send_control_status_locked` 的 DC 分支恢复为一行 `gst_webrtc_data_channel_send_string(control_channel, message.dump().c_str());`（删除 snapshot_mirror、buffered_bytes、replaceable、skip_dc 和 `pending_control_status[kind]` 的镜像写入）。
  - 遥测 trace 中删除 `mock_skipped_dc_telemetry`、`mock_skipped_dc_vcu_snapshot` 字段，删除这两个成员。
  - 控制服务创建处 `mock_dual_control_enabled() ? 40 : 100` 改为 `100`，删除其上方注释。
- [ ] **Step 4：Windows 客户端**（`windows_native_viewer.cpp`）
  - 删除 `config.mock_dual_control = mock_control;`；删除 `wss_dc_mirror` 参数及计算；`NativeControlCourier` 构造改为 `std::make_unique<detail::NativeControlCourier>(current)`；状态显示删除"双路已发送"分支（`control_path` 只区分切换中、DataChannel、WSS 回退、未知）；evidence 中删除 `mock_dual_control`、`mock_dual_last_vehicle_path` 字段；车辆结果日志删除 `vehicle_received_path`。
- [ ] **Step 5：测试与构建**
  - `control_transport_tests.cpp`：删除所有使用镜像参数调用 `admit(...)` 的断言（约 200–216 行）、`mock_dual_sender_allowed` / `owns_wss_dc_mirror` 用例。
  - `control_transport_integration_tests.cpp`：删除 `mock_dual_control_survives_direct_packet_loss()`、`mock_wss_dc_mirror_survives_cloud_carrier_loss()`、`legacy_mock_peer_does_not_receive_wss_dc_mirrors()`，夹具中的 `mock_dual`、`wss_dc_peer` 及其所有使用（`pump()` 开头的统计块、offer 中两项能力），构造函数改成 Interfaces 中的新签名并同步所有调用处；`main()` 删除 `--mock-dual-only`、`--mock-wss-dc-only` 两个分支。
  - `feedback_vehicle_method_tests.cpp`、`native_control_publication_tests.cpp`、`control_tests.cpp`、`ice_vehicle_callbacks.test.py`：删除所有引用镜像/背压的用例与期望。
  - `CMakeLists.txt`：删除 `mine-teleop-mock-dual-control-integration-tests`、`mine-teleop-mock-wss-dc-integration-tests` 两个 `add_test`，`set_tests_properties` 中同步删除（保留 `mine-teleop-native-mock-estop-integration-tests`）。
  - `git rm cpp/include/mine_teleop/detail/mock_dc_backpressure.hpp`
- [ ] **Step 6：验证**

Run：本部分开头的 grep（此时只允许 `mock_media_status_v1` 和 `native_mock_media_evidence` 相关结果残留，留给 Task 8）；然后 Task 0 Step 2。
Expected：CTest 全部通过。

- [ ] **Step 7：提交**

```bash
git add -A cpp CMakeLists.txt
git commit -m "Remove the experimental DC/WSS mock mirror" -m "Co-Authored-By: Claude <noreply@anthropic.com>"
```

## Task 8：恢复 mock 媒体门控的单载体形态（已改写，原为 B4 补丁）

执行时确认：门控里的重复副本记账（`native_mock_media_evidence`、`validated_`、重复副本放行）是提交 `0e0617b`
"Mirror scoped mock media evidence on direct channel" 为了接受 DC 上的第二份媒体状态才加的。DC 媒体状态镜像在
Task 7 删除后，不会再有重复或过期副本到达，原计划的"忽略过期副本"补丁不可达，按 YAGNI 不做。
本任务改为：对 `0e0617b` 在门控头文件和门控测试上的改动做反向应用（`git show 0e0617b -- <两个文件> | git apply -R`），
恢复单载体的原始行为，并删除 `test_mirrored_media_evidence`。Task 7 里已先行删除车端和 viewer 的 DC 媒体状态镜像（见下）。

## Task 9：统一隔离 mock 判定（A6）与车端 mock 证明（B6）

**Files:**
- Create: `cpp/include/mine_teleop/detail/isolated_mock_bench.hpp`
- Modify: `cpp/src/webrtc_media.cpp`（RTP/ICE marker 探针、mock 遥测 trace 的三处 `control_log_commands && test_bench_ && mock` 判断；offer）
- Modify: `cpp/apps/windows_native_viewer.cpp`（offer 处理，`if (mock_control) {` 分支，约 1216 行）
- Test: Create `cpp/tests/isolated_mock_bench_tests.cpp`；Modify `CMakeLists.txt`

**Interfaces:**
- Produces: `inline constexpr std::string_view kIsolatedMockBenchCapability = "isolated-mock-bench-v1";`、`bool isolated_mock_bench(std::string_view vehicle_id, std::string_view adapter_type, double max_speed_kph, std::string_view commissioning_mode)`、`bool isolated_mock_bench_offer(const nlohmann::json& offer_payload)`。

- [ ] **Step 1：写失败的测试** `cpp/tests/isolated_mock_bench_tests.cpp`

```cpp
#include "mine_teleop/detail/isolated_mock_bench.hpp"
#include <iostream>
#include <stdexcept>
using namespace mine_teleop::detail;
void require(bool value, const char* why) { if (!value) throw std::runtime_error(why); }
int main() {
  try {
    require(isolated_mock_bench("test_bench_001", "mock", 0.0, "bench"), "isolated bench rejected");
    require(!isolated_mock_bench("vehicle-001", "mock", 0.0, "bench"), "non-bench id accepted");
    require(!isolated_mock_bench("test_bench_001", "can", 0.0, "bench"), "real adapter accepted");
    require(!isolated_mock_bench("test_bench_001", "mock", 0.1, "bench"), "non-zero speed accepted");
    require(!isolated_mock_bench("test_bench_001", "mock", 0.0, "field"), "field commissioning accepted");
    require(isolated_mock_bench_offer({{"isolated_mock_bench_capability", "isolated-mock-bench-v1"}}), "attested offer rejected");
    require(!isolated_mock_bench_offer({{"isolated_mock_bench_capability", ""}}), "empty attestation accepted");
    require(!isolated_mock_bench_offer({{"isolated_mock_bench_capability", 1}}), "malformed attestation accepted");
    require(!isolated_mock_bench_offer(nlohmann::json::object()), "missing attestation accepted");
    std::cout << "PASS isolated mock bench attestation\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
```

`CMakeLists.txt`（`MINE_TELEOP_BUILD_TESTS OR MINE_TELEOP_BUILD_PORTABLE_CONTROL_TESTS` 块内）：

```cmake
  add_executable(mine-teleop-isolated-mock-bench-tests cpp/tests/isolated_mock_bench_tests.cpp)
  target_link_libraries(mine-teleop-isolated-mock-bench-tests PRIVATE mine_teleop_runtime)
  mine_teleop_enable_warnings(mine-teleop-isolated-mock-bench-tests)
  add_test(NAME mine-teleop-isolated-mock-bench-tests COMMAND mine-teleop-isolated-mock-bench-tests)
```

- [ ] **Step 2：运行，确认失败**（头文件不存在，编译失败）

- [ ] **Step 3：创建头文件** `cpp/include/mine_teleop/detail/isolated_mock_bench.hpp`

```cpp
#pragma once

// One predicate for every isolated zero-speed mock bench behavior: bench-only
// diagnostics and the attestation a native mock client requires before it
// sends any control. Real adapters, non-bench IDs or non-zero speed never pass.
#include <nlohmann/json.hpp>
#include <string_view>

namespace mine_teleop::detail {

inline constexpr std::string_view kIsolatedMockBenchCapability = "isolated-mock-bench-v1";

inline bool isolated_mock_bench(std::string_view vehicle_id, std::string_view adapter_type,
                                double max_speed_kph, std::string_view commissioning_mode) {
  return vehicle_id.starts_with("test_bench_") && adapter_type == "mock" &&
      max_speed_kph == 0.0 && commissioning_mode == "bench";
}

inline bool isolated_mock_bench_offer(const nlohmann::json& payload) {
  if (!payload.is_object() || !payload.contains("isolated_mock_bench_capability")) return false;
  const auto& value = payload.at("isolated_mock_bench_capability");
  return value.is_string() && value.get_ref<const std::string&>() == kIsolatedMockBenchCapability;
}

}  // namespace mine_teleop::detail
```

- [ ] **Step 4：车端使用统一判定**（`webrtc_media.cpp`）
  - include 新头文件；在 `Impl` 中新增成员 `const bool isolated_mock_trace;`，在 `Impl` 构造函数初始化列表中初始化为
    `config.runtime.control_log_commands && detail::isolated_mock_bench(config.vehicle_id, config.vehicle_adapter.type, config.field_safety.max_speed_kph, config.field_safety.commissioning_mode)`。
  - 把三处 `config.runtime.control_log_commands && config.vehicle_id.starts_with("test_bench_") && config.vehicle_adapter.type == "mock"` 全部替换为 `isolated_mock_trace`。
  - offer 负载中加入：`{"isolated_mock_bench_capability", detail::isolated_mock_bench(self->config.vehicle_id, self->config.vehicle_adapter.type, self->config.field_safety.max_speed_kph, self->config.field_safety.commissioning_mode) ? detail::kIsolatedMockBenchCapability : std::string_view{}}`。
- [ ] **Step 5：Windows 客户端要求车端证明**

在 viewer 处理 offer 的 `if (mock_control) {` 分支最前面加入：

```cpp
                if (!detail::isolated_mock_bench_offer(payload))
                  throw std::runtime_error("车端未声明隔离 mock 台架，已拒绝开启控制；请改用只读观看");
```

并在文件顶部 include `mine_teleop/detail/isolated_mock_bench.hpp`。

- [ ] **Step 6：运行**

Run: Task 0 Step 2；`grep -n 'starts_with("test_bench_")' cpp/src/webrtc_media.cpp` → 无输出。
Expected：`mine-teleop-isolated-mock-bench-tests` PASS，其余不回归。

- [ ] **Step 7：提交**

```bash
git add cpp/include/mine_teleop/detail/isolated_mock_bench.hpp cpp/src/webrtc_media.cpp cpp/apps/windows_native_viewer.cpp cpp/tests/isolated_mock_bench_tests.cpp CMakeLists.txt
git commit -m "Require vehicle attestation before native mock control" -m "Co-Authored-By: Claude <noreply@anthropic.com>"
```

## Task 10：接收端析构不再与 libnice 回调竞争（B7）

**Files:**
- Modify: `cpp/tests/native_receiver_core.hpp`（`Receiver` 的 `start()`、`stop()`、`on_pair`；macOS 下的 `InterfaceBinding::on_candidate`）

- [ ] **Step 1：给 libnice 回调加带锁的所有者状态**

在 `class Receiver` 的私有区加入：

```cpp
  // libnice emits from its own context; it is not joined by the pipeline's
  // NULL transition. The callback holds this lock while it uses the owner, so
  // stop() cannot return while an invocation is still running.
  struct NiceCallbackState {
    std::mutex mutex;
    Receiver* owner{};
    InterfaceBinding* binding{};
  };
  std::shared_ptr<NiceCallbackState> nice_callbacks_ = std::make_shared<NiceCallbackState>();
  static void release_nice_callbacks(gpointer data, GClosure*) {
    delete static_cast<std::shared_ptr<NiceCallbackState>*>(data);
  }
  static void on_pair_owned(NiceAgent* agent, guint stream, guint component, NiceCandidate* local,
                            NiceCandidate* remote, gpointer data) {
    const auto state = *static_cast<std::shared_ptr<NiceCallbackState>*>(data);
    std::lock_guard lock(state->mutex);
    if (state->owner) on_pair(agent, stream, component, local, remote, state->owner);
  }
#ifdef __APPLE__
  static void on_candidate_owned(NiceAgent* agent, NiceCandidate* candidate, gpointer data) {
    const auto state = *static_cast<std::shared_ptr<NiceCallbackState>*>(data);
    std::lock_guard lock(state->mutex);
    if (state->binding) InterfaceBinding::on_candidate(agent, candidate, state->binding);
  }
#endif
```

- [ ] **Step 2：连接与断开改用该状态**
  - `start()` 中设置 `nice_callbacks_->owner = this;`；macOS 分支设置 `nice_callbacks_->binding = interface_binding_.get();`，并把两处 `g_signal_connect(nice_, ...)` 改为：

```cpp
    g_signal_connect_data(nice_, "new-selected-pair-full", G_CALLBACK(on_pair_owned),
        new std::shared_ptr<NiceCallbackState>(nice_callbacks_), release_nice_callbacks, GConnectFlags(0));
```

    （`new-candidate-full` 同理，回调为 `on_candidate_owned`。）
  - `stop()` 中，在 `gst_element_set_state(pipeline_, GST_STATE_NULL);` 之前加入：

```cpp
    {
      std::lock_guard lock(nice_callbacks_->mutex);
      nice_callbacks_->owner = nullptr;
      nice_callbacks_->binding = nullptr;
    }
```

    并把 `g_signal_handlers_disconnect_by_data(nice_, this)` 和对 `interface_binding_.get()` 的断开，改为 `g_signal_handlers_disconnect_matched(nice_, G_SIGNAL_MATCH_FUNC, 0, 0, nullptr, reinterpret_cast<gpointer>(on_pair_owned), nullptr)`（macOS 再对 `on_candidate_owned` 做一次）。
- [ ] **Step 3：运行**

Run: Task 0 Step 2（`mine-teleop-native-receiver-*` 系列在 Linux 上运行）。
Expected：全部通过。

- [ ] **Step 4：提交**

```bash
git add cpp/tests/native_receiver_core.hpp
git commit -m "Fence native receiver libnice callbacks during teardown" -m "Co-Authored-By: Claude <noreply@anthropic.com>"
```

## Task 11：清理受删除影响的文档

**Files:**
- Modify: `docs/windows-native-viewer.md`、`docs/native-receiver-bench.md`
- Modify: 引用 DC/WSS 双路或紧凑格式结论的证据文件（用 `grep -ln "双路\|镜像\|紧凑\|compact\|mirror" docs/evidence/*.md` 找出）

- [ ] **Step 1：更新使用文档**：删除对紧凑格式、DC/WSS 镜像、DC 媒体状态镜像的使用说明；在 viewer 文档中写明 mock 控制需要车端声明 `isolated-mock-bench-v1`。
- [ ] **Step 2：标注失效证据**：在每个相关证据文件开头加入一行：

```markdown
> **已失效（2026-11-04）：** 本记录基于已删除的 DC/WSS 镜像或紧凑格式实现，其双路对比结论不再适用，仅作历史参考。见 docs/64-transient-link-hold-design.md。
```

- [ ] **Step 3：提交**

```bash
git add docs
git commit -m "Mark mirror-era evidence obsolete and update bench docs" -m "Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

# 第三部分　瞬态链路保持

## Task 12：`TransientHoldPolicy` 纯逻辑组件

**Files:**
- Create: `cpp/include/mine_teleop/detail/transient_hold.hpp`
- Create: `cpp/tests/transient_hold_policy_tests.cpp`
- Modify: `CMakeLists.txt`

**Interfaces（后续任务都依赖这些名字）:**
- `inline constexpr std::string_view kTransientHoldCapability = "transient-hold-v1";`
- `struct TransientHoldConfig { bool enabled; int max_hold_ms; double hold_brake; int resume_fresh_commands; int budget_window_ms; int max_holds_per_window; int max_hold_time_per_window_ms; void validate() const; };`
- `enum class TransientCause : unsigned { ControlGap, LateCommands, DriverVideoStale, VehicleVideoStale };`、`std::string_view to_string(TransientCause)`
- `enum class TransientHoldAction { None, Enter, Hold, Resume, Escalate };`
- `class TransientHoldPolicy`：`set_cause(TransientCause, bool present, std::int64_t now_ms)`、`note_fresh_command(std::int64_t now_ms)`、`tick(std::int64_t now_ms)`、`escalate_active(std::int64_t now_ms, std::string_view reason)`、`escalation_cleared()`、`reset()`、`active()`、`escalated()`、`causes()`、`escalation_reason()`、`config()`、`nlohmann::json status(std::int64_t now_ms, bool negotiated) const`
- `struct TransientHoldOffer { int control_tolerance_ms; int video_tolerance_ms; int max_hold_ms; };`、`std::optional<TransientHoldOffer> parse_transient_hold_offer(const nlohmann::json& payload)`
- `enum class VehicleVideoLaneState { Fresh, Stale, Fault };`、`VehicleVideoLaneState classify_vehicle_video_lane(bool disabled, bool faulted, bool has_freshness, bool invalid_sample_blocked, bool encoded_fresh, unsigned consecutive_anomalies, bool awaiting_recovery_frames)`

- [ ] **Step 1：写失败的测试** `cpp/tests/transient_hold_policy_tests.cpp`

```cpp
#include "mine_teleop/detail/transient_hold.hpp"
#include <iostream>
#include <stdexcept>
using namespace mine_teleop::detail;
namespace {
void require(bool value, const char* why) { if (!value) throw std::runtime_error(why); }
TransientHoldConfig enabled() { TransientHoldConfig config; config.enabled = true; return config; }
void resume_within_window() {
  TransientHoldPolicy policy(enabled());
  require(policy.set_cause(TransientCause::ControlGap, true, 1000) == TransientHoldAction::Enter, "gap did not enter hold");
  require(policy.tick(1500) == TransientHoldAction::Hold, "hold ended early");
  require(policy.set_cause(TransientCause::ControlGap, false, 1550) == TransientHoldAction::Hold, "clearing a cause ended the hold");
  require(policy.note_fresh_command(1550) == TransientHoldAction::Hold, "one fresh command resumed");
  require(policy.note_fresh_command(1600) == TransientHoldAction::Resume && !policy.active(), "second fresh command did not resume");
  require(policy.status(1600, true).at("last").at("outcome") == "resumed", "resume outcome not recorded");
}
void cause_reappearing_resets_fresh_count() {
  TransientHoldPolicy policy(enabled());
  policy.set_cause(TransientCause::DriverVideoStale, true, 0);
  policy.set_cause(TransientCause::DriverVideoStale, false, 100);
  require(policy.note_fresh_command(100) == TransientHoldAction::Hold, "first fresh command after clearing");
  policy.set_cause(TransientCause::DriverVideoStale, true, 150);
  policy.set_cause(TransientCause::DriverVideoStale, false, 200);
  require(policy.note_fresh_command(200) == TransientHoldAction::Hold, "fresh count survived a new stall");
  require(policy.note_fresh_command(250) == TransientHoldAction::Resume, "no resume after two consecutive fresh commands");
}
void escalate_at_max_hold() {
  TransientHoldPolicy policy(enabled());
  policy.set_cause(TransientCause::ControlGap, true, 1000);
  require(policy.tick(1699) == TransientHoldAction::Hold, "escalated before 700 ms");
  require(policy.tick(1700) == TransientHoldAction::Escalate && policy.escalated(), "did not escalate at 700 ms");
  require(policy.escalation_reason() == "hold_duration_exceeded", "wrong escalation reason");
  require(policy.set_cause(TransientCause::ControlGap, true, 1800) == TransientHoldAction::None, "escalated policy re-entered a hold");
  policy.escalation_cleared();
  require(policy.set_cause(TransientCause::ControlGap, true, 5000) == TransientHoldAction::Enter, "cleared policy cannot hold again");
}
void budget_count_time_and_window() {
  TransientHoldPolicy count(enabled());
  for (int i = 0; i < 6; ++i) {
    const std::int64_t t = i * 1000;
    require(count.set_cause(TransientCause::LateCommands, true, t) == TransientHoldAction::Enter, "budgeted hold rejected");
    count.set_cause(TransientCause::LateCommands, false, t + 50);
    count.note_fresh_command(t + 50);
    require(count.note_fresh_command(t + 100) == TransientHoldAction::Resume, "budgeted hold did not resume");
  }
  require(count.set_cause(TransientCause::LateCommands, true, 6000) == TransientHoldAction::Escalate &&
      count.escalation_reason() == "budget_count_exceeded", "seventh hold within the window was not escalated");
  // Each hold lasts 650 ms. Four completed holds use 2600 ms of the 3000 ms budget,
  // so the fifth hold must escalate mid-hold (at +450 ms), not at the next entry.
  TransientHoldPolicy time(enabled());
  int escalated_at = -1;
  for (int i = 0; i < 6 && escalated_at < 0; ++i) {
    const std::int64_t t = i * 2000;
    if (time.set_cause(TransientCause::ControlGap, true, t) == TransientHoldAction::Escalate) { escalated_at = i; break; }
    for (std::int64_t step = 50; step <= 650 && escalated_at < 0; step += 50)
      if (time.tick(t + step) == TransientHoldAction::Escalate) escalated_at = i;
    if (escalated_at >= 0) break;
    time.set_cause(TransientCause::ControlGap, false, t + 650);
    time.note_fresh_command(t + 650); time.note_fresh_command(t + 660);
  }
  require(escalated_at == 4 && time.escalation_reason() == "budget_time_exceeded",
      "cumulative hold time budget was not enforced mid-hold on the fifth hold");
  TransientHoldPolicy window(enabled());
  for (int i = 0; i < 6; ++i) {
    const std::int64_t t = i * 1000;
    window.set_cause(TransientCause::LateCommands, true, t);
    window.set_cause(TransientCause::LateCommands, false, t + 10);
    window.note_fresh_command(t + 10); window.note_fresh_command(t + 20);
  }
  require(window.set_cause(TransientCause::LateCommands, true, 66000) == TransientHoldAction::Enter, "budget window did not slide");
}
void fault_clock_and_disabled() {
  TransientHoldPolicy fault(enabled());
  fault.set_cause(TransientCause::VehicleVideoStale, true, 100);
  require(fault.escalate_active(200, "fault_during_hold") == TransientHoldAction::Escalate &&
      fault.escalation_reason() == "fault_during_hold", "fault during hold not escalated");
  TransientHoldPolicy clock(enabled());
  clock.set_cause(TransientCause::ControlGap, true, 100);
  require(clock.tick(50) == TransientHoldAction::Escalate && clock.escalation_reason() == "clock_reversed", "clock reversal did not fail closed");
  TransientHoldPolicy disabled;
  require(disabled.set_cause(TransientCause::ControlGap, true, 0) == TransientHoldAction::None && !disabled.active(), "disabled policy held");
}
void validation_offer_and_lanes() {
  bool threw = false;
  try { auto bad = enabled(); bad.max_hold_ms = 99; TransientHoldPolicy policy(bad); } catch (const std::invalid_argument&) { threw = true; }
  require(threw, "max_hold_ms below range accepted");
  threw = false;
  try { auto bad = enabled(); bad.hold_brake = 0.31; bad.validate(); } catch (const std::invalid_argument&) { threw = true; }
  require(threw, "hold_brake above 0.3 accepted");
  const nlohmann::json offer = {{"transient_hold_capability", "transient-hold-v1"},
      {"transient_hold", {{"control_tolerance_ms", 1000}, {"video_tolerance_ms", 1200}, {"max_hold_ms", 700}}}};
  const auto parsed = parse_transient_hold_offer(offer);
  require(parsed && parsed->control_tolerance_ms == 1000 && parsed->video_tolerance_ms == 1200 && parsed->max_hold_ms == 700, "valid offer rejected");
  auto wide = offer; wide["transient_hold"]["video_tolerance_ms"] = 4500;
  require(parse_transient_hold_offer(wide).has_value(), "maximum video tolerance rejected");
  for (auto broken : {nlohmann::json::object(), nlohmann::json{{"transient_hold_capability", "transient-hold-v2"}}}) require(!parse_transient_hold_offer(broken), "unsupported offer accepted");
  auto low = offer; low["transient_hold"]["control_tolerance_ms"] = 399;
  require(!parse_transient_hold_offer(low), "control tolerance below 400 accepted");
  auto text = offer; text["transient_hold"]["max_hold_ms"] = "700";
  require(!parse_transient_hold_offer(text), "non-integer field accepted");
  require(classify_vehicle_video_lane(false, false, true, false, true, 0, false) == VehicleVideoLaneState::Fresh, "fresh lane misclassified");
  require(classify_vehicle_video_lane(false, false, true, false, false, 0, false) == VehicleVideoLaneState::Stale, "aged lane not stale");
  require(classify_vehicle_video_lane(false, false, true, false, true, 2, false) == VehicleVideoLaneState::Stale, "dropped frames not stale");
  require(classify_vehicle_video_lane(false, false, true, false, true, 0, true) == VehicleVideoLaneState::Stale, "recovering lane not stale");
  require(classify_vehicle_video_lane(false, false, true, true, true, 0, false) == VehicleVideoLaneState::Fault, "invalid samples not a fault");
  require(classify_vehicle_video_lane(true, false, true, false, true, 0, false) == VehicleVideoLaneState::Fault, "disabled lane not a fault");
  require(classify_vehicle_video_lane(false, true, true, false, true, 0, false) == VehicleVideoLaneState::Fault, "faulted lane not a fault");
  require(classify_vehicle_video_lane(false, false, false, false, true, 0, false) == VehicleVideoLaneState::Fault, "missing freshness not a fault");
}
}  // namespace
int main() {
  try {
    resume_within_window(); cause_reappearing_resets_fresh_count(); escalate_at_max_hold();
    budget_count_time_and_window(); fault_clock_and_disabled(); validation_offer_and_lanes();
    std::cout << "PASS transient hold policy: resume, escalation, budget, clock, offer and lane classification\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
```

`CMakeLists.txt`（`MINE_TELEOP_BUILD_TESTS OR MINE_TELEOP_BUILD_PORTABLE_CONTROL_TESTS` 块内）：

```cmake
  add_executable(mine-teleop-transient-hold-policy-tests cpp/tests/transient_hold_policy_tests.cpp)
  target_link_libraries(mine-teleop-transient-hold-policy-tests PRIVATE mine_teleop_runtime)
  mine_teleop_enable_warnings(mine-teleop-transient-hold-policy-tests)
  add_test(NAME mine-teleop-transient-hold-policy-tests COMMAND mine-teleop-transient-hold-policy-tests)
```

- [ ] **Step 2：运行，确认失败**（头文件不存在，编译失败）

- [ ] **Step 3：实现** `cpp/include/mine_teleop/detail/transient_hold.hpp`

```cpp
#pragma once

// Bounded tolerance for transient control-link and video freshness loss.
// Pure logic: no I/O and no locking. The owner serializes every call.
// See docs/64-transient-link-hold-design.md.
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <deque>
#include <initializer_list>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace mine_teleop::detail {

inline constexpr std::string_view kTransientHoldCapability = "transient-hold-v1";

struct TransientHoldConfig {
  bool enabled{false};
  int max_hold_ms{700};
  double hold_brake{0.15};
  int resume_fresh_commands{2};
  int budget_window_ms{60000};
  int max_holds_per_window{6};
  int max_hold_time_per_window_ms{3000};

  void validate() const {
    const auto fail = [](const char* text) { throw std::invalid_argument(text); };
    if (max_hold_ms < 100 || max_hold_ms > 1500) fail("control.transient_hold.max_hold_ms must be 100..1500");
    if (!(hold_brake >= 0.0 && hold_brake <= 0.3)) fail("control.transient_hold.hold_brake must be 0..0.3");
    if (resume_fresh_commands < 1 || resume_fresh_commands > 5)
      fail("control.transient_hold.resume_fresh_commands must be 1..5");
    if (budget_window_ms < 10000 || budget_window_ms > 600000)
      fail("control.transient_hold.budget_window_ms must be 10000..600000");
    if (max_holds_per_window < 1 || max_holds_per_window > 100)
      fail("control.transient_hold.max_holds_per_window must be 1..100");
    if (max_hold_time_per_window_ms < 100 || max_hold_time_per_window_ms > budget_window_ms)
      fail("control.transient_hold.max_hold_time_per_window_ms must be 100..budget_window_ms");
  }
};

enum class TransientCause : unsigned {
  ControlGap = 1U << 0U,
  LateCommands = 1U << 1U,
  DriverVideoStale = 1U << 2U,
  VehicleVideoStale = 1U << 3U,
};

inline constexpr std::string_view to_string(TransientCause cause) {
  switch (cause) {
    case TransientCause::ControlGap: return "control_gap";
    case TransientCause::LateCommands: return "late_commands";
    case TransientCause::DriverVideoStale: return "driver_video_stale";
    case TransientCause::VehicleVideoStale: return "vehicle_video_stale";
  }
  return "unknown";
}

enum class TransientHoldAction { None, Enter, Hold, Resume, Escalate };

class TransientHoldPolicy {
 public:
  explicit TransientHoldPolicy(TransientHoldConfig config = {}) : config_(config) {
    if (config_.enabled) config_.validate();
  }

  [[nodiscard]] const TransientHoldConfig& config() const { return config_; }
  [[nodiscard]] bool active() const { return entered_ms_.has_value(); }
  [[nodiscard]] bool escalated() const { return escalated_; }
  [[nodiscard]] unsigned causes() const { return causes_; }
  [[nodiscard]] std::string_view escalation_reason() const { return last_reason_; }

  TransientHoldAction set_cause(TransientCause cause, bool present, std::int64_t now_ms) {
    if (!config_.enabled || escalated_) return TransientHoldAction::None;
    if (!observe_clock(now_ms)) return escalate(now_ms, "clock_reversed");
    const auto bit = static_cast<unsigned>(cause);
    if (!present) {
      causes_ &= ~bit;
      return active() ? TransientHoldAction::Hold : TransientHoldAction::None;
    }
    causes_ |= bit;
    fresh_commands_ = 0;
    if (active()) return TransientHoldAction::Hold;
    prune(now_ms);
    entered_ms_ = now_ms;
    if (static_cast<int>(history_.size()) + 1 > config_.max_holds_per_window)
      return escalate(now_ms, "budget_count_exceeded");
    if (hold_time_in_window(now_ms) >= config_.max_hold_time_per_window_ms)
      return escalate(now_ms, "budget_time_exceeded");
    return TransientHoldAction::Enter;
  }

  TransientHoldAction note_fresh_command(std::int64_t now_ms) {
    if (!config_.enabled || escalated_ || !active()) return TransientHoldAction::None;
    if (!observe_clock(now_ms)) return escalate(now_ms, "clock_reversed");
    if (causes_ != 0) {
      fresh_commands_ = 0;
      return TransientHoldAction::Hold;
    }
    if (++fresh_commands_ < config_.resume_fresh_commands) return TransientHoldAction::Hold;
    finish(now_ms, "resumed", {});
    return TransientHoldAction::Resume;
  }

  TransientHoldAction tick(std::int64_t now_ms) {
    if (!config_.enabled || escalated_ || !active()) return TransientHoldAction::None;
    if (!observe_clock(now_ms)) return escalate(now_ms, "clock_reversed");
    if (now_ms - *entered_ms_ >= config_.max_hold_ms) return escalate(now_ms, "hold_duration_exceeded");
    if (hold_time_in_window(now_ms) > config_.max_hold_time_per_window_ms)
      return escalate(now_ms, "budget_time_exceeded");
    return TransientHoldAction::Hold;
  }

  // A fault-class cause or a transport epoch change while a hold is active.
  TransientHoldAction escalate_active(std::int64_t now_ms, std::string_view reason) {
    if (!active() || escalated_) return TransientHoldAction::None;
    return escalate(now_ms, reason);
  }

  // The existing braking wait recovered; new holds may start again.
  void escalation_cleared() {
    escalated_ = false;
    causes_ = 0;
    fresh_commands_ = 0;
  }

  void reset() {
    entered_ms_.reset();
    last_now_.reset();
    causes_ = 0;
    fresh_commands_ = 0;
    escalated_ = false;
    history_.clear();
    last_ = nullptr;
    last_reason_.clear();
  }

  [[nodiscard]] nlohmann::json status(std::int64_t now_ms, bool negotiated) const {
    const auto window_start = now_ms - config_.budget_window_ms;
    int holds = active() ? 1 : 0;
    for (const auto& episode : history_)
      if (episode.start_ms + episode.duration_ms >= window_start) ++holds;
    return {{"enabled", config_.enabled && negotiated}, {"active", active()}, {"causes", cause_names(causes_)},
            {"entered_at_monotonic_ms", entered_ms_ ? nlohmann::json(*entered_ms_) : nlohmann::json(nullptr)},
            {"elapsed_ms", entered_ms_ && now_ms >= *entered_ms_ ? nlohmann::json(now_ms - *entered_ms_) : nlohmann::json(nullptr)},
            {"max_hold_ms", config_.max_hold_ms}, {"hold_brake", config_.hold_brake},
            {"holds_in_window", holds}, {"hold_time_in_window_ms", hold_time_in_window(now_ms)},
            {"budget_window_ms", config_.budget_window_ms}, {"escalated", escalated_}, {"last", last_}};
  }

 private:
  struct Episode {
    std::int64_t start_ms;
    std::int64_t duration_ms;
  };

  static nlohmann::json cause_names(unsigned causes) {
    auto names = nlohmann::json::array();
    for (const auto cause : {TransientCause::ControlGap, TransientCause::LateCommands,
                             TransientCause::DriverVideoStale, TransientCause::VehicleVideoStale})
      if (causes & static_cast<unsigned>(cause)) names.push_back(std::string(to_string(cause)));
    return names;
  }

  bool observe_clock(std::int64_t now_ms) {
    if (now_ms < 0 || (last_now_ && now_ms < *last_now_)) return false;
    last_now_ = now_ms;
    return true;
  }

  void prune(std::int64_t now_ms) {
    const auto window_start = now_ms - config_.budget_window_ms;
    while (!history_.empty() && history_.front().start_ms + history_.front().duration_ms < window_start)
      history_.pop_front();
  }

  [[nodiscard]] std::int64_t hold_time_in_window(std::int64_t now_ms) const {
    const auto window_start = now_ms - config_.budget_window_ms;
    std::int64_t total = 0;
    for (const auto& episode : history_) {
      const auto begin = std::max(episode.start_ms, window_start);
      const auto end = episode.start_ms + episode.duration_ms;
      if (end > begin) total += end - begin;
    }
    if (entered_ms_ && now_ms > *entered_ms_) total += now_ms - std::max(*entered_ms_, window_start);
    return total;
  }

  TransientHoldAction escalate(std::int64_t now_ms, std::string_view reason) {
    if (!entered_ms_) entered_ms_ = last_now_.value_or(std::max<std::int64_t>(0, now_ms));
    finish(std::max(now_ms, *entered_ms_), "escalated", reason);
    escalated_ = true;
    return TransientHoldAction::Escalate;
  }

  void finish(std::int64_t now_ms, std::string_view outcome, std::string_view reason) {
    const auto start = *entered_ms_;
    const auto duration = std::max<std::int64_t>(0, now_ms - start);
    last_ = {{"causes", cause_names(causes_)}, {"duration_ms", duration}, {"outcome", std::string(outcome)},
             {"escalation_reason", reason.empty() ? nlohmann::json(nullptr) : nlohmann::json(std::string(reason))}};
    last_reason_ = std::string(reason);
    history_.push_back({start, duration});
    while (static_cast<int>(history_.size()) > config_.max_holds_per_window + 1) history_.pop_front();
    entered_ms_.reset();
    causes_ = 0;
    fresh_commands_ = 0;
  }

  TransientHoldConfig config_;
  std::optional<std::int64_t> entered_ms_;
  std::optional<std::int64_t> last_now_;
  unsigned causes_{0};
  int fresh_commands_{0};
  bool escalated_{false};
  std::deque<Episode> history_;
  nlohmann::json last_ = nullptr;
  std::string last_reason_;
};

struct TransientHoldOffer {
  int control_tolerance_ms{0};
  int video_tolerance_ms{0};
  int max_hold_ms{0};
};

// Driver-side admission of the vehicle capability. Anything unexpected keeps
// both endpoints on the existing behavior.
inline std::optional<TransientHoldOffer> parse_transient_hold_offer(const nlohmann::json& payload) {
  try {
    if (!payload.is_object() || !payload.contains("transient_hold_capability") ||
        !payload.at("transient_hold_capability").is_string() ||
        payload.at("transient_hold_capability").get_ref<const std::string&>() != kTransientHoldCapability ||
        !payload.contains("transient_hold") || !payload.at("transient_hold").is_object())
      return std::nullopt;
    const auto& details = payload.at("transient_hold");
    const auto integer = [&](const char* key, std::int64_t low, std::int64_t high) -> std::optional<int> {
      if (!details.contains(key) || !details.at(key).is_number_integer()) return std::nullopt;
      const auto value = details.at(key).get<std::int64_t>();
      if (value < low || value > high) return std::nullopt;
      return static_cast<int>(value);
    };
    const auto control = integer("control_tolerance_ms", 400, 2000);
    const auto video = integer("video_tolerance_ms", 1, 4500);
    const auto hold = integer("max_hold_ms", 100, 1500);
    if (!control || !video || !hold) return std::nullopt;
    return TransientHoldOffer{*control, *video, *hold};
  } catch (const nlohmann::json::exception&) {
    return std::nullopt;
  }
}

enum class VehicleVideoLaneState { Fresh, Stale, Fault };

// Stale or IDR-wait drops are transient; invalid samples and lane faults are not.
inline VehicleVideoLaneState classify_vehicle_video_lane(bool disabled, bool faulted, bool has_freshness,
    bool invalid_sample_blocked, bool encoded_fresh, unsigned consecutive_anomalies, bool awaiting_recovery_frames) {
  if (disabled || faulted || !has_freshness || invalid_sample_blocked) return VehicleVideoLaneState::Fault;
  if (!encoded_fresh || consecutive_anomalies != 0 || awaiting_recovery_frames) return VehicleVideoLaneState::Stale;
  return VehicleVideoLaneState::Fresh;
}

}  // namespace mine_teleop::detail
```

- [ ] **Step 4：运行，确认通过**

Run: Task 0 Step 2（`mine-teleop-transient-hold-policy-tests`）。
Expected：`PASS transient hold policy: ...`。

- [ ] **Step 5：提交**

```bash
git add cpp/include/mine_teleop/detail/transient_hold.hpp cpp/tests/transient_hold_policy_tests.cpp CMakeLists.txt
git commit -m "Add the transient link hold policy" -m "Co-Authored-By: Claude <noreply@anthropic.com>"
```

## Task 13：车端配置（`transient_hold`、`bridge_apply_timeout_ms`）

**Files:**
- Modify: `cpp/include/mine_teleop/core.hpp`（`struct ControlConfig`，约 449–455 行）
- Modify: `cpp/src/core.cpp`（`load_vehicle_config` 的 control 段，约 2073–2090 行；`redacted_summary`，约 1960 行；`create_vehicle_adapter`，约 3060 行）
- Modify: `configs/vehicle-agent.three-machine.field.yaml`、`configs/vehicle-agent.ccg2-8m.yaml`
- Test: `cpp/tests/core_tests.cpp`

**Interfaces:**
- Produces: `ControlConfig::transient_hold`（`detail::TransientHoldConfig`）、`ControlConfig::bridge_apply_timeout_ms`（`int`，加载后总是有值）。

- [ ] **Step 1：写失败的配置测试**

在 `core_tests.cpp` 中加入（测试自带 `variant` lambda，基于 dev 配置生成临时 YAML），并在 `main()` 中调用：

```cpp
void test_transient_hold_configuration() {
  const auto base = std::filesystem::path("configs/vehicle-agent.dev.yaml");
  const auto source = [&] { std::ifstream in(base); std::stringstream text; text << in.rdbuf(); return text.str(); }();
  // The dev YAML contains exactly "  control_timeout_ms: 800\n" under control:.
  const auto variant = [&](const std::string& control_timeout, const std::string& extra, const std::string& name) {
    auto text = source;
    const std::string anchor = "  control_timeout_ms: 800\n";
    const auto at = text.find(anchor);
    expect(at != std::string::npos, "dev vehicle YAML control_timeout_ms anchor missing");
    text.replace(at, anchor.size(), "  control_timeout_ms: " + control_timeout + "\n" + extra);
    const auto path = std::filesystem::temp_directory_path() / name;
    std::ofstream(path) << text;
    return path;
  };
  const auto defaults = mine_teleop::load_vehicle_config(base);
  expect(!defaults.control.transient_hold.enabled && defaults.control.bridge_apply_timeout_ms == defaults.control.control_timeout_ms,
      "missing transient_hold must be disabled and the bridge timeout must default to control_timeout_ms");
  const auto enabled = mine_teleop::load_vehicle_config(variant("1200",
      "  bridge_apply_timeout_ms: 800\n  transient_hold:\n    enabled: true\n", "transient-ok.yaml"));
  expect(enabled.control.transient_hold.enabled && enabled.control.transient_hold.max_hold_ms == 700 &&
      enabled.control.transient_hold.hold_brake == 0.15 && enabled.control.bridge_apply_timeout_ms == 800,
      "transient_hold defaults or bridge timeout not loaded");
  expect_throws([&] { static_cast<void>(mine_teleop::load_vehicle_config(variant("800",
      "  transient_hold:\n    enabled: true\n", "transient-too-long.yaml"))); },
      "hold window longer than the hard watchdog allows was accepted");
  expect_throws([&] { static_cast<void>(mine_teleop::load_vehicle_config(variant("1200",
      "  transient_hold:\n    enabled: true\n    hold_brake: 0.4\n", "transient-brake.yaml"))); },
      "hold_brake above 0.3 was accepted");
  expect_throws([&] { static_cast<void>(mine_teleop::load_vehicle_config(variant("1200",
      "  bridge_apply_timeout_ms: 5000\n", "bridge-too-long.yaml"))); },
      "bridge timeout longer than control_timeout_ms was accepted");
}
```

说明：dev 配置的 `degraded_timeout_ms` 为 300，所以 `control_timeout_ms: 800` 下开启默认 700 ms 的保持时 `300 + 700 + 100 > 800`，必须拒绝；改为 1200 后合法。`expect_throws` 是 `core_tests.cpp` 已有的辅助函数（第 53 行）。文件顶部按需补 `#include <fstream>`、`#include <sstream>`、`#include <filesystem>`。

- [ ] **Step 2：运行，确认失败**（`transient_hold` / `bridge_apply_timeout_ms` 字段不存在，编译失败）

- [ ] **Step 3：实现配置结构与解析**
  - `core.hpp` 顶部 include `mine_teleop/detail/transient_hold.hpp`；`ControlConfig` 增加：

```cpp
  int bridge_apply_timeout_ms{800};            // Chassis bridge apply watchdog (process liveness).
  detail::TransientHoldConfig transient_hold;  // Disabled unless the YAML block enables it.
```

  - `load_vehicle_config` 中，在现有 control timing 校验之后加入：

```cpp
  config.control.bridge_apply_timeout_ms =
      optional<int>(control, "bridge_apply_timeout_ms", config.control.control_timeout_ms);
  if (config.control.bridge_apply_timeout_ms < 20 || config.control.bridge_apply_timeout_ms > 60000 ||
      config.control.bridge_apply_timeout_ms > config.control.control_timeout_ms)
    throw std::runtime_error("control.bridge_apply_timeout_ms must be 20..60000 and no greater than control_timeout_ms");
  if (const auto hold = control["transient_hold"]; hold) {
    auto& value = config.control.transient_hold;
    value.enabled = optional<bool>(hold, "enabled", false);
    value.max_hold_ms = optional<int>(hold, "max_hold_ms", 700);
    value.hold_brake = optional<double>(hold, "hold_brake", 0.15);
    value.resume_fresh_commands = optional<int>(hold, "resume_fresh_commands", 2);
    value.budget_window_ms = optional<int>(hold, "budget_window_ms", 60000);
    value.max_holds_per_window = optional<int>(hold, "max_holds_per_window", 6);
    value.max_hold_time_per_window_ms = optional<int>(hold, "max_hold_time_per_window_ms", 3000);
    if (value.enabled) {
      try { value.validate(); } catch (const std::invalid_argument& error) { throw std::runtime_error(error.what()); }
      if (config.control.degraded_timeout_ms + value.max_hold_ms + 100 > config.control.control_timeout_ms)
        throw std::runtime_error(
            "control.transient_hold requires degraded_timeout_ms + max_hold_ms + 100 <= control_timeout_ms");
    }
  }
```

  - 现有 `speed_feedback_timeout_ms > control_timeout_ms` 校验（约 2326 行）改为与 `bridge_apply_timeout_ms` 比较，错误信息同步改为 `bridge_apply_timeout_ms`。
  - `create_vehicle_adapter` 中传给 `DynamicLibraryVehicleAdapter` 的 `config.control.control_timeout_ms` 改为 `config.control.bridge_apply_timeout_ms`。
  - `redacted_summary` 增加 `{"control_timeout_ms", control.control_timeout_ms}, {"bridge_apply_timeout_ms", control.bridge_apply_timeout_ms}, {"transient_hold", {{"enabled", control.transient_hold.enabled}, {"max_hold_ms", control.transient_hold.max_hold_ms}, {"hold_brake", control.transient_hold.hold_brake}}}`（键名若与已有输出冲突则合并到已有对象）。
- [ ] **Step 4：更新现场配置**

在 `vehicle-agent.three-machine.field.yaml`、`vehicle-agent.ccg2-8m.yaml` 的 `control:` 段中，把 `control_timeout_ms: 800` 改为下面的内容（保留其他键）：

```yaml
  control_timeout_ms: 1200
  bridge_apply_timeout_ms: 800
  transient_hold:
    enabled: true
    max_hold_ms: 700
    hold_brake: 0.15
    resume_fresh_commands: 2
    budget_window_ms: 60000
    max_holds_per_window: 6
    max_hold_time_per_window_ms: 3000
```

- [ ] **Step 5：运行**

Run: Task 0 Step 2；另外在镜像内执行 `/build/mine-teleop config-check --config /src/configs/vehicle-agent.three-machine.field.yaml`（二进制名以构建产物为准）。
Expected：`mine-teleop-core-tests` 通过；两个现场配置 `config-check` 通过。

- [ ] **Step 6：提交**

```bash
git add cpp/include/mine_teleop/core.hpp cpp/src/core.cpp cpp/tests/core_tests.cpp configs/vehicle-agent.three-machine.field.yaml configs/vehicle-agent.ccg2-8m.yaml
git commit -m "Load transient hold and bridge timeout vehicle settings" -m "Co-Authored-By: Claude <noreply@anthropic.com>"
```

## Task 14：命令字段 `transient_hold` 与 `critical_video_transient`

**Files:**
- Modify: `cpp/include/mine_teleop/core.hpp`（`struct ControlCommand`）
- Modify: `cpp/src/core.cpp`（`ControlCommand::to_json`、`from_json`、`validate`）
- Test: `cpp/tests/core_tests.cpp`

**Interfaces:**
- Produces: `ControlCommand::transient_hold_capable`（`bool`）、`ControlCommand::critical_video_transient`（`bool`）。JSON：能力为真时输出 `"transient_hold": "transient-hold-v1"` 和 `"critical_video_transient": <bool>`；为假时两个键都不输出。

- [ ] **Step 1：写失败的测试**

```cpp
void test_transient_hold_command_fields() {
  auto value = command(1, 1000);
  value.transient_hold_capable = true; value.critical_video_ready = false; value.critical_video_transient = true;
  const auto json = value.to_json();
  expect(json.at("transient_hold") == "transient-hold-v1" && json.at("critical_video_transient") == true, "fields not serialized");
  const auto parsed = mine_teleop::ControlCommand::from_json(json);
  expect(parsed.transient_hold_capable && parsed.critical_video_transient, "fields not parsed");
  auto legacy = command(2, 1000).to_json();
  expect(!legacy.contains("transient_hold") && !legacy.contains("critical_video_transient"), "legacy command gained new keys");
  expect(!mine_teleop::ControlCommand::from_json(legacy).transient_hold_capable, "legacy command negotiated hold");
  auto wrong = json; wrong["transient_hold"] = "transient-hold-v2";
  expect_throws([&] { static_cast<void>(mine_teleop::ControlCommand::from_json(wrong)); }, "unknown capability accepted");
  auto contradictory = json; contradictory["critical_video_ready"] = true;
  expect_throws([&] { static_cast<void>(mine_teleop::ControlCommand::from_json(contradictory)); },
      "transient video flag accepted while video was ready");
}
```

（`command(seq, t)` 为 `core_tests.cpp` 已有的命令构造辅助函数。）

- [ ] **Step 2：运行，确认失败**（字段不存在，编译失败）

- [ ] **Step 3：实现**
  - `ControlCommand` 在 `recovery_brake_revision` 之后增加：

```cpp
  // Signed only after the vehicle advertised transient-hold-v1.
  bool transient_hold_capable{false};
  bool critical_video_transient{false};
```

  - `core.cpp` include `mine_teleop/detail/transient_hold.hpp`。`to_json()` 末尾（`return` 之前）加入：

```cpp
  if (transient_hold_capable) {
    value["transient_hold"] = std::string(detail::kTransientHoldCapability);
    value["critical_video_transient"] = critical_video_transient;
  }
```

    （`value` 是 `to_json` 中已有的局部对象，见 `core.cpp:1029`。）
  - `from_json()` 在 `command.control_token = ...` 之后加入：

```cpp
    if (value.contains("transient_hold")) {
      const auto& marker = value.at("transient_hold");
      if (!marker.is_string() || marker.get_ref<const std::string&>() != detail::kTransientHoldCapability)
        throw std::invalid_argument("transient_hold capability is invalid");
      command.transient_hold_capable = true;
    }
    if (value.contains("critical_video_transient")) {
      if (!value.at("critical_video_transient").is_boolean())
        throw std::invalid_argument("critical_video_transient must be a boolean");
      command.critical_video_transient = value.at("critical_video_transient").get<bool>();
    }
```

  - `ControlCommand::validate()` 末尾加入：

```cpp
  if (critical_video_transient && (!transient_hold_capable || critical_video_ready))
    throw std::invalid_argument("critical_video_transient requires transient_hold and critical_video_ready=false");
```

- [ ] **Step 4：运行**

Run: Task 0 Step 2。
Expected：`mine-teleop-core-tests` 通过，其余不回归（旧命令 JSON 不变）。

- [ ] **Step 5：提交**

```bash
git add cpp/include/mine_teleop/core.hpp cpp/src/core.cpp cpp/tests/core_tests.cpp
git commit -m "Add signed transient hold command fields" -m "Co-Authored-By: Claude <noreply@anthropic.com>"
```

## Task 15：车端控制服务接入瞬态保持

**Files:**
- Modify: `cpp/include/mine_teleop/core.hpp`（`class VehicleControlService`）
- Modify: `cpp/src/core.cpp`（构造函数、`receive_session_profile` 接受分支、`receive_command`、`tick`、`set_control_braking_wait`、`evaluate_control_watchdog`、`control_timing`）
- Create: `cpp/tests/transient_hold_service_tests.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: Task 12 的 `TransientHoldPolicy`、`TransientCause`、`TransientHoldAction`；Task 13 的 `ControlConfig::transient_hold`；Task 14 的命令字段。
- Produces（公开方法，Task 16 使用）：`bool transient_hold_enabled() const`、`bool transient_hold_active() const`、`bool driver_critical_video_fault() const`、`void observe_vehicle_video_transient(bool stale, std::int64_t timestamp_ms)`、`void note_transport_epoch_changed(std::int64_t timestamp_ms)`、`std::vector<Json> drain_transient_hold_events()`。`control_timing()` 新增 `transient_hold`、`driver_critical_video_transient`、`vehicle_video_transient` 三个键。

- [ ] **Step 1：写失败的服务测试** `cpp/tests/transient_hold_service_tests.cpp`

```cpp
#include "mine_teleop/core.hpp"
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

namespace {
using mine_teleop::ControlCommand;
using mine_teleop::ControlRecoveryCause;
using mine_teleop::Json;
using mine_teleop::MockVehicleAdapter;
using mine_teleop::SafetyState;
using mine_teleop::VehicleControlService;
void require(bool value, const std::string& why) { if (!value) throw std::runtime_error(why); }

mine_teleop::SessionControlProfileRequest profile_request(std::int64_t now) {
  mine_teleop::SessionControlProfileRequest request;
  request.vehicle_id = "vehicle-001"; request.driver_id = "driver-001"; request.session_id = "session-001";
  request.seq = 1; request.sent_at_utc_ms = now; request.control_token = "token";
  request.profile.profile_version = mine_teleop::kSessionControlProfileVersion;
  request.profile.consecutive_late_frame_limit = 3;
  request.profile.target_speed_kph = 20.0; request.profile.max_motor_torque_nm = 100.0;
  request.profile.max_brake_pressure_bar = 100.0; request.profile.service_brake_pressure_bar = 30.0;
  request.profile.hard_brake_pressure_bar = 100.0; request.profile.max_steering_angle_deg = 3.0;
  request.profile.speed_pid_kp = 1.0; request.profile.speed_pid_ki = 0.2; request.profile.speed_pid_kd = 0.0;
  request.profile.speed_pid_derivative_filter_tau_ms = 100.0; request.profile.speed_pid_max_dt_ms = 100;
  return request;
}

struct Harness {
  std::int64_t mono{1000}, utc{100000};
  MockVehicleAdapter* vehicle{nullptr};
  std::unique_ptr<VehicleControlService> service;
  std::uint64_t seq{0};
  explicit Harness(bool enabled = true) {
    auto config = mine_teleop::load_vehicle_config("configs/vehicle-agent.dev.yaml");
    config.control.control_timeout_ms = 1200;
    config.control.transient_hold.enabled = enabled;
    auto adapter = std::make_unique<MockVehicleAdapter>(); vehicle = adapter.get();
    service = std::make_unique<VehicleControlService>(config, "driver-001", "session-001", "token",
        std::move(adapter), 100, [this] { return mono; });
    service->start(utc);
    require(service->receive_session_profile(profile_request(utc), utc).accepted, "profile rejected");
  }
  mine_teleop::ReceiveResult send(double throttle, double steering = 0.0, std::int64_t age = 20, bool capable = true,
                                  bool video_ready = true, bool video_transient = false, bool link_ready = true, bool estop = false) {
    ControlCommand value;
    value.vehicle_id = "vehicle-001"; value.driver_id = "driver-001"; value.session_id = "session-001";
    value.seq = ++seq; value.sent_at_utc_ms = utc - age; value.gear = "D"; value.throttle = throttle;
    value.steering = steering; value.control_token = "token"; value.estop = estop;
    value.operator_active = throttle > 0.0 || steering != 0.0;
    value.critical_video_ready = video_ready; value.critical_video_transient = video_transient;
    value.control_link_ready = link_ready; value.transient_hold_capable = capable;
    return service->receive_command(value, utc);
  }
  void wait(std::int64_t ms) { for (std::int64_t step = 0; step < ms; step += 50) { mono += 50; utc += 50; service->tick(utc); } }
  Json hold() const { return service->control_timing(utc).at("transient_hold"); }
  Json timing() const { return service->control_timing(utc); }
  mine_teleop::VehicleTelemetry output() { return vehicle->read_telemetry(); }
};

void short_gap_does_not_hold() {
  Harness h; require(h.send(0.5).accepted, "initial command rejected");
  h.wait(250);
  require(!h.hold().at("active").get<bool>() && h.output().throttle_feedback > 0.0, "250 ms gap entered a hold");
}
void gap_holds_with_light_brake_and_resumes_without_rearm() {
  Harness h; require(h.send(0.5, 0.05).accepted, "initial command rejected");
  h.wait(500);
  require(h.hold().at("active").get<bool>() && h.hold().at("causes").at(0) == "control_gap", "500 ms gap did not hold");
  auto out = h.output();
  require(out.throttle_feedback == 0.0 && out.brake_feedback >= 0.15 && out.steering_feedback > 0.0, "hold output wrong");
  require(!h.timing().at("rearm_required").get<bool>() && !h.timing().at("control_braking_wait").get<bool>(),
      "hold demanded rearm or braking wait");
  const auto first = h.send(0.5, 0.05);
  require(first.accepted && h.hold().at("active").get<bool>(), "first fresh command ended the hold");
  bool warned = false; for (const auto& warning : first.warnings) warned = warned || warning == "transient_hold_traction_withheld";
  require(warned, "traction-withheld warning missing");
  require(h.send(0.5, 0.05).accepted && !h.hold().at("active").get<bool>(), "second fresh command did not resume");
  require(h.output().throttle_feedback > 0.0, "resumed command did not apply traction");
  require(h.hold().at("last").at("outcome") == "resumed", "resume not recorded");
}
void long_gap_escalates_on_watchdog_tick() {
  Harness h; require(h.send(0.5).accepted, "initial command rejected");
  h.wait(1050);
  require(h.timing().at("control_braking_wait").get<bool>() && h.output().brake_feedback >= 1.0, "1000 ms gap did not escalate");
  require(h.hold().at("last").at("escalation_reason") == "hold_duration_exceeded", "wrong escalation reason");
  require(!h.send(0.5).accepted, "motion accepted without rearm after escalation");
}
void late_commands_hold_then_resume() {
  Harness h; require(h.send(0.5).accepted, "initial command rejected");
  for (int i = 0; i < 3; ++i) { h.wait(50); static_cast<void>(h.send(0.5, 0.0, 250)); }
  require(h.hold().at("active").get<bool>(), "three late commands did not hold");
  require(!h.timing().at("control_braking_wait").get<bool>(), "late commands caused braking wait");
  h.wait(50); static_cast<void>(h.send(0.5)); h.wait(50);
  require(h.send(0.5).accepted && !h.hold().at("active").get<bool>(), "fresh commands did not resume after late ones");
}
void driver_video_transient_holds_and_fault_brakes() {
  Harness h; require(h.send(0.5).accepted, "initial command rejected");
  static_cast<void>(h.send(0.5, 0.0, 20, true, false, true));
  require(h.hold().at("active").get<bool>() && !h.timing().at("control_braking_wait").get<bool>(), "video stall did not hold");
  static_cast<void>(h.send(0.5)); static_cast<void>(h.send(0.5));
  require(!h.hold().at("active").get<bool>(), "video recovery did not resume");
  static_cast<void>(h.send(0.5, 0.0, 20, true, false, false));
  require(h.timing().at("control_braking_wait").get<bool>(), "video fault did not brake immediately");
}
void vehicle_video_transient_holds() {
  Harness h; require(h.send(0.5).accepted, "initial command rejected");
  h.service->observe_vehicle_video_transient(true, h.utc);
  require(h.hold().at("active").get<bool>(), "vehicle video stall did not hold");
  static_cast<void>(h.send(0.5)); static_cast<void>(h.send(0.5));
  require(h.hold().at("active").get<bool>(), "hold resumed while vehicle video was still stale");
  h.service->observe_vehicle_video_transient(false, h.utc);
  static_cast<void>(h.send(0.5)); static_cast<void>(h.send(0.5));
  require(!h.hold().at("active").get<bool>(), "vehicle video recovery did not resume");
}
void link_not_ready_is_a_fault() {
  Harness h; require(h.send(0.5).accepted, "initial command rejected");
  static_cast<void>(h.send(0.0, 0.0, 20, true, true, false, false));
  require(h.timing().at("control_braking_wait").get<bool>(), "control_link_ready=false did not brake");
}
void estop_during_hold() {
  Harness h; require(h.send(0.5).accepted, "initial command rejected");
  h.wait(400);
  static_cast<void>(h.send(0.0, 0.0, 20, true, true, false, true, true));
  require(h.service->safety_state() == SafetyState::Estop, "estop during hold not latched");
}
void brake_floor_survives_hard_timeout(bool enabled) {
  Harness h(enabled); require(h.send(0.5).accepted, "initial command rejected");
  h.service->set_control_braking_wait(true, h.utc, ControlRecoveryCause::ControlLink);
  h.wait(1250);
  require(h.service->safety_state() == SafetyState::TimeoutBrake, "hard timeout not reached");
  require(h.output().brake_feedback >= 1.0, "brake dropped below the braking-wait floor at hard timeout");
}
void hard_timeout_boundary() {
  Harness h; require(h.send(0.5).accepted, "initial command rejected");
  h.wait(1150);
  require(h.service->safety_state() != SafetyState::TimeoutBrake, "hard timeout before 1200 ms");
  h.wait(50);
  require(h.service->safety_state() == SafetyState::TimeoutBrake, "hard timeout not at 1200 ms");
}
void legacy_without_capability() {
  Harness h; require(h.send(0.5, 0.0, 20, false).accepted, "initial command rejected");
  h.wait(500);
  require(!h.hold().at("active").get<bool>(), "legacy session entered a hold");
  static_cast<void>(h.send(0.5, 0.0, 20, false));
  require(h.timing().at("control_braking_wait").get<bool>(), "legacy gap behavior changed");
}
void budget_escalation() {
  Harness h; require(h.send(0.5).accepted, "initial command rejected");
  for (int i = 0; i < 6; ++i) {
    h.wait(350);
    require(h.hold().at("active").get<bool>(), "budgeted gap did not hold");
    static_cast<void>(h.send(0.5)); static_cast<void>(h.send(0.5));
    require(!h.hold().at("active").get<bool>(), "budgeted hold did not resume");
  }
  h.wait(350);
  require(h.timing().at("control_braking_wait").get<bool>() &&
      h.hold().at("last").at("escalation_reason") == "budget_count_exceeded", "seventh hold in a minute did not escalate");
}
}  // namespace

int main() {
  try {
    short_gap_does_not_hold(); gap_holds_with_light_brake_and_resumes_without_rearm(); long_gap_escalates_on_watchdog_tick();
    late_commands_hold_then_resume(); driver_video_transient_holds_and_fault_brakes(); vehicle_video_transient_holds();
    link_not_ready_is_a_fault(); estop_during_hold(); brake_floor_survives_hard_timeout(true); brake_floor_survives_hard_timeout(false);
    hard_timeout_boundary(); legacy_without_capability(); budget_escalation();
    std::cout << "PASS vehicle transient hold: gap/late/video holds, light brake, resume, escalation, faults, floor, legacy\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
```

`CMakeLists.txt`（与 `mine-teleop-core-tests` 同一块，工作目录为源码根目录，因为测试读取 `configs/`）：

```cmake
  add_executable(mine-teleop-transient-hold-service-tests cpp/tests/transient_hold_service_tests.cpp)
  target_link_libraries(mine-teleop-transient-hold-service-tests PRIVATE mine_teleop_runtime)
  mine_teleop_enable_warnings(mine-teleop-transient-hold-service-tests)
  add_test(NAME mine-teleop-transient-hold-service-tests COMMAND mine-teleop-transient-hold-service-tests)
  set_tests_properties(mine-teleop-transient-hold-service-tests PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
```

- [ ] **Step 2：运行，确认失败**（`observe_vehicle_video_transient` 不存在，编译失败）

- [ ] **Step 3：声明成员**（`core.hpp`）

公开区追加：

```cpp
  [[nodiscard]] bool transient_hold_enabled() const;
  [[nodiscard]] bool transient_hold_active() const { return transient_hold_.active(); }
  [[nodiscard]] bool driver_critical_video_fault() const {
    return !driver_critical_video_ready_ && !driver_critical_video_transient_;
  }
  void observe_vehicle_video_transient(bool stale, std::int64_t timestamp_ms);
  // A transport epoch change during a hold escalates with its own reason.
  void note_transport_epoch_changed(std::int64_t timestamp_ms);
  [[nodiscard]] std::vector<Json> drain_transient_hold_events();
```

私有区追加：

```cpp
  bool handle_transient_action(detail::TransientHoldAction action, ControlRecoveryCause cause, std::int64_t timestamp_ms);
  [[nodiscard]] ControlRecoveryCause transient_recovery_cause() const;
  void apply_hold_output();
  void record_transient_event(std::string_view event);
  detail::TransientHoldPolicy transient_hold_;
  bool transient_hold_negotiated_{false};
  bool driver_critical_video_transient_{false};
  bool vehicle_video_transient_{false};
  std::optional<ControlOutput> hold_output_;
  double brake_floor_{0.0};
  std::deque<Json> transient_hold_events_;
```

- [ ] **Step 4：实现辅助方法**（`core.cpp`，放在 `set_control_braking_wait` 之前）

构造函数初始化列表加入 `transient_hold_(config.control.transient_hold),`（放在 `telemetry_interval_ms_` 之后，与成员声明顺序一致）。

```cpp
bool VehicleControlService::transient_hold_enabled() const {
  return transient_hold_.config().enabled && transient_hold_negotiated_ && active_session_profile_.has_value();
}

ControlRecoveryCause VehicleControlService::transient_recovery_cause() const {
  const auto causes = transient_hold_.causes();
  const auto video = static_cast<unsigned>(detail::TransientCause::DriverVideoStale) |
      static_cast<unsigned>(detail::TransientCause::VehicleVideoStale);
  return (causes & video) != 0 ? ControlRecoveryCause::CriticalVideo : ControlRecoveryCause::ControlLink;
}

void VehicleControlService::record_transient_event(std::string_view event) {
  auto details = transient_hold_.status(monotonic_clock_(), transient_hold_negotiated_);
  details["event"] = std::string(event);
  if (transient_hold_events_.size() >= 16) transient_hold_events_.pop_front();
  transient_hold_events_.push_back(std::move(details));
}

std::vector<Json> VehicleControlService::drain_transient_hold_events() {
  std::vector<Json> events(transient_hold_events_.begin(), transient_hold_events_.end());
  transient_hold_events_.clear();
  return events;
}

void VehicleControlService::apply_hold_output() {
  if (!hold_output_ || !active_session_profile_ || adapter_safe_stop_active_ ||
      (safety_.state() != SafetyState::ControlActive && safety_.state() != SafetyState::Degraded)) return;
  adapter_->apply_safe_stop(*hold_output_, {});
}

// Returns true when the action escalated to the existing braking wait.
bool VehicleControlService::handle_transient_action(detail::TransientHoldAction action,
    ControlRecoveryCause cause, std::int64_t timestamp_ms) {
  switch (action) {
    case detail::TransientHoldAction::Enter: {
      ControlOutput output;
      output.gear = last_effective_command_ ? last_effective_command_->gear : adapter_->read_telemetry().gear;
      if (output.gear.empty()) output.gear = "N";
      output.steering = last_effective_command_ ? last_effective_command_->steering : 0.0;
      const double operator_brake = last_effective_command_ && active_session_profile_
          ? vehicle_normalized_brake_request(last_effective_command_->brake,
                active_session_profile_->max_brake_pressure_bar, max_brake_pressure_bar_)
          : 0.0;
      output.brake = std::max(operator_brake, transient_hold_.config().hold_brake);
      hold_output_ = output;
      brake_floor_ = std::max(brake_floor_, transient_hold_.config().hold_brake);
      apply_hold_output();
      record_transient_event("vehicle_transient_hold_entered");
      return false;
    }
    case detail::TransientHoldAction::Resume:
      hold_output_.reset();
      brake_floor_ = 0.0;
      record_transient_event("vehicle_transient_hold_resumed");
      return false;
    case detail::TransientHoldAction::Escalate:
      hold_output_.reset();
      record_transient_event("vehicle_transient_hold_escalated");
      set_control_braking_wait(true, timestamp_ms, cause);
      return true;
    case detail::TransientHoldAction::None:
    case detail::TransientHoldAction::Hold:
      return false;
  }
  return false;
}

void VehicleControlService::observe_vehicle_video_transient(bool stale, std::int64_t timestamp_ms) {
  vehicle_video_transient_ = stale;
  if (!transient_hold_enabled() || control_braking_wait_) return;
  handle_transient_action(transient_hold_.set_cause(detail::TransientCause::VehicleVideoStale, stale, monotonic_clock_()),
      ControlRecoveryCause::CriticalVideo, timestamp_ms);
}

void VehicleControlService::note_transport_epoch_changed(std::int64_t timestamp_ms) {
  if (!transient_hold_.active()) return;
  handle_transient_action(transient_hold_.escalate_active(monotonic_clock_(), "transport_epoch_changed"),
      ControlRecoveryCause::ControlLink, timestamp_ms);
}
```

在 Step 1 的测试文件中补一个用例，并加进 `main()`：

```cpp
void epoch_change_escalates_with_its_own_reason() {
  Harness h; require(h.send(0.5).accepted, "initial command rejected");
  h.wait(400);
  require(h.hold().at("active").get<bool>(), "gap did not hold");
  h.service->note_transport_epoch_changed(h.utc);
  require(h.timing().at("control_braking_wait").get<bool>() &&
      h.hold().at("last").at("escalation_reason") == "transport_epoch_changed", "epoch change did not escalate with its reason");
}
```

（`vehicle_normalized_brake_request` 是 `core.cpp` 中 `receive_command` 已在使用的函数。）

- [ ] **Step 5：修改 `set_control_braking_wait`**

```cpp
void VehicleControlService::set_control_braking_wait(bool waiting, std::int64_t timestamp_ms, ControlRecoveryCause cause) {
  if (control_braking_wait_ == waiting) return;
  if (waiting && transient_hold_.active() &&
      transient_hold_.escalate_active(monotonic_clock_(), "fault_during_hold") == detail::TransientHoldAction::Escalate) {
    hold_output_.reset();
    record_transient_event("vehicle_transient_hold_escalated");
  }
  control_braking_wait_ = waiting;
  if (waiting) { recovery_brake_hold_ = true; recovery_cause_ = cause; brake_floor_ = 1.0; }
  else transient_hold_.escalation_cleared();
  timing_rearm_required_ = true;
  safety_.require_input_rearm();
  evaluate_control_watchdog();
  if (waiting) apply_recovery_brake();
  static_cast<void>(timestamp_ms);
}
```

- [ ] **Step 6：修改 `evaluate_control_watchdog`**

在函数开头 `safety_.tick(monotonic_clock_());` 改为：

```cpp
  const auto now = monotonic_clock_();
  safety_.tick(now);
  if (transient_hold_enabled() && !control_braking_wait_ && !transient_hold_.escalated() &&
      safety_.state() == SafetyState::Degraded &&
      handle_transient_action(transient_hold_.set_cause(detail::TransientCause::ControlGap, true, now),
          ControlRecoveryCause::ControlLink, 0))
    return;  // Escalation already re-entered this function through set_control_braking_wait.
```

并把原有的输出分支改为：

```cpp
      if (safety_.state() == SafetyState::Degraded &&
          (control_braking_wait_ || recovery_brake_hold_)) {
        apply_recovery_brake();
      } else if (safety_.state() == SafetyState::Degraded && transient_hold_enabled() &&
                 transient_hold_.active() && hold_output_) {
        apply_hold_output();
      } else {
        auto output = safety_.current_output(now);
        // The floor raises only the brake value; latching still follows the
        // staged profile because full_emergency_brake was computed above.
        if (safety_.state() == SafetyState::TimeoutBrake) output.brake = std::max(output.brake, brake_floor_);
        adapter_->apply_safe_stop(output, stop_context);
      }
```

- [ ] **Step 7：修改 `tick`**

把 `evaluate_control_watchdog(); apply_recovery_brake();` 两行替换为：

```cpp
  evaluate_control_watchdog();
  if (transient_hold_enabled()) {
    const auto cause = transient_recovery_cause();
    handle_transient_action(transient_hold_.tick(monotonic_clock_()), cause, timestamp_ms);
  }
  apply_recovery_brake();
  if (transient_hold_enabled() && transient_hold_.active()) apply_hold_output();
```

- [ ] **Step 8：修改 `receive_command`**
  - 断流后首包（约 3477–3480 行）改为：

```cpp
    if (active_session_profile_ && safety_.state() == SafetyState::Degraded &&
        safety_.last_valid_receive_ms() && arrival_monotonic_ms - *safety_.last_valid_receive_ms() >=
            control_limits_.at("read_only_control_safety").at("degraded_timeout_ms").get<int>()) {
      if (transient_hold_enabled())
        handle_transient_action(transient_hold_.set_cause(detail::TransientCause::ControlGap, true, arrival_monotonic_ms),
            ControlRecoveryCause::ControlLink, timestamp_ms);
      else
        set_control_braking_wait(true, timestamp_ms, ControlRecoveryCause::ControlLink);
    }
```

  - 迟到/断流拒绝分支（约 3483–3495 行）改为：

```cpp
  if (!result.accepted || !result.command) {
    const bool timing_violation = result.reason == "command_gap_exceeded" ||
        (result.reason == "command_age_exceeded" &&
         receiver_.consecutive_late_frames() >= receiver_.consecutive_late_frame_limit());
    if (timing_violation && active_session_profile_ &&
        (safety_.state() == SafetyState::ControlActive || safety_.state() == SafetyState::Degraded)) {
      if (transient_hold_enabled()) {
        handle_transient_action(transient_hold_.set_cause(detail::TransientCause::LateCommands, true, arrival_monotonic_ms),
            ControlRecoveryCause::ControlLink, timestamp_ms);
      } else {
        timing_rearm_required_ = true;
        safety_.require_input_rearm();
        set_control_braking_wait(true, timestamp_ms, ControlRecoveryCause::ControlLink);
        evaluate_control_watchdog();
        result.issue_code = "control_input_rearm_required";
      }
    }
    return result;
  }
```

  - 就绪标志块（约 3511–3529 行）改为：

```cpp
  if (!effective.estop) {
    transient_hold_negotiated_ = effective.transient_hold_capable;
    const bool hold_mode = transient_hold_enabled();
    driver_critical_video_ready_ = !critical_video_required_ || effective.critical_video_ready;
    driver_critical_video_transient_ = hold_mode && !driver_critical_video_ready_ && effective.critical_video_transient;
    driver_time_sync_ready_ = effective.control_time_sync_ready;
    operator_input_ready_ = effective.operator_input_ready;
    driver_control_link_ready_ = effective.control_link_ready;
    recovery_samples_healthy_ = effective.recovery_sample_healthy;
    const bool historical_brake_event = effective.recovery_brake_revision != recovery_brake_revision_;
    recovery_bad_sample_revision_ = effective.recovery_bad_sample_revision;
    recovery_brake_revision_ = effective.recovery_brake_revision;
    if (historical_brake_event) set_control_braking_wait(true, timestamp_ms, ControlRecoveryCause::ReadinessHistory);
    const bool video_fault = !driver_critical_video_ready_ && !driver_critical_video_transient_;
    if (video_fault || !driver_time_sync_ready_ || !operator_input_ready_ || !driver_control_link_ready_)
      set_control_braking_wait(true, timestamp_ms, !driver_time_sync_ready_ ? ControlRecoveryCause::TimeSynchronization
          : !driver_control_link_ready_ ? ControlRecoveryCause::ControlLink
          : !operator_input_ready_ ? ControlRecoveryCause::InputLease : ControlRecoveryCause::CriticalVideo);
    if (hold_mode && !control_braking_wait_) {
      handle_transient_action(transient_hold_.set_cause(detail::TransientCause::ControlGap, false, arrival_monotonic_ms),
          ControlRecoveryCause::ControlLink, timestamp_ms);
      handle_transient_action(transient_hold_.set_cause(detail::TransientCause::LateCommands, false, arrival_monotonic_ms),
          ControlRecoveryCause::ControlLink, timestamp_ms);
      handle_transient_action(transient_hold_.set_cause(detail::TransientCause::DriverVideoStale,
          driver_critical_video_transient_, arrival_monotonic_ms), ControlRecoveryCause::CriticalVideo, timestamp_ms);
      handle_transient_action(transient_hold_.note_fresh_command(arrival_monotonic_ms),
          ControlRecoveryCause::ControlLink, timestamp_ms);
    }
  }
```

  - 重按拒绝条件（约 3530 行）中的 `safety_.state() == SafetyState::Degraded` 改为
    `(safety_.state() == SafetyState::Degraded && !(transient_hold_enabled() && !control_braking_wait_ && !timing_rearm_required_))`。
  - 输出分支（约 3628–3649 行）改为：

```cpp
      const bool fresh_motion = !control_braking_wait_ &&
          (result.command->throttle > 1e-9 || std::abs(result.command->steering) > 1e-9);
      const bool holding = transient_hold_enabled() && transient_hold_.active() && hold_output_;
      if (control_braking_wait_ || (recovery_brake_hold_ && !fresh_motion)) {
        apply_recovery_brake(result.command->gear);
        result.command->brake = safety_command.brake = 1.0;
        result.warnings.emplace_back("control_recovery_safety_brake_applied");
      } else if (holding) {
        hold_output_->brake = std::max(safety_command.brake, transient_hold_.config().hold_brake);
        apply_hold_output();
        result.warnings.emplace_back("transient_hold_traction_withheld");
      } else {
        adapter_->apply_control(*result.command);
        if (fresh_motion) { recovery_brake_hold_ = false; brake_floor_ = 0.0; }
      }
```

    并把其后的 `last_effective_command_ = *result.command;` 改为 `if (!holding) last_effective_command_ = *result.command;`（保持期间挡位和转向不跟随新命令）。
  - `receive_session_profile` 的接受分支（`active_session_profile_ = request.profile;` 之后）加入 `transient_hold_.reset(); hold_output_.reset();`。
- [ ] **Step 9：修改 `control_timing`**

把 `rearm_required` 一项改为：

```cpp
          {"rearm_required", control_braking_wait_ || timing_rearm_required_ ||
              (safety_.state() == SafetyState::Degraded && receiver_.consecutive_late_frame_limit() == 1 &&
               !(transient_hold_enabled() && !transient_hold_.escalated()))},
```

并在返回对象中追加：

```cpp
          {"transient_hold", transient_hold_.status(monotonic_clock_(), transient_hold_negotiated_ && active_session_profile_.has_value())},
          {"driver_critical_video_transient", driver_critical_video_transient_},
          {"vehicle_video_transient", vehicle_video_transient_},
```

- [ ] **Step 10：运行**

Run: Task 0 Step 2。
Expected：`mine-teleop-transient-hold-service-tests` PASS；`mine-teleop-core-tests`、`mine-teleop-control-tests`、`mine-teleop-control-recovery-tests`、`mine-teleop-recovery-enforcer-tests` 等现有用例全部通过（未协商时行为不变）。若现有用例断言了"制动等待后硬超时输出 0.3"，那是本设计修正的缺陷：把该断言改为 `>= 1.0`，并在提交说明中写明原因。

- [ ] **Step 11：提交**

```bash
git add cpp/include/mine_teleop/core.hpp cpp/src/core.cpp cpp/tests/transient_hold_service_tests.cpp CMakeLists.txt
git commit -m "Hold traction through transient vehicle control gaps" -m "Escalate to the existing braking wait after 700 ms or when the hold budget is exhausted, and keep the brake floor through the hard timeout." -m "Co-Authored-By: Claude <noreply@anthropic.com>"
```

## Task 16：车端媒体运行时接入（能力声明、视频分类、事件）

**Files:**
- Modify: `cpp/include/mine_teleop/detail/gstreamer_freshness.hpp`（`EncodedSendFreshness` 增加故障类修订号）
- Modify: `cpp/src/webrtc_media.cpp`（offer 约 1270 行；`enforce_control_recovery` 约 3550–3620 行）
- Test: `cpp/tests/media_freshness_gstreamer_tests.cpp`、`cpp/tests/ice_vehicle_callbacks.test.py`

**Interfaces:**
- Consumes: Task 12 的 `classify_vehicle_video_lane`、`kTransientHoldCapability`；Task 15 的公开方法。
- Produces: `EncodedSendFreshness::invalid_brake_revision`（`std::atomic<std::uint64_t>`）。

- [ ] **Step 1：写失败的测试**
  - `media_freshness_gstreamer_tests.cpp`：在已有"非法样本"用例中追加断言 `require(state.invalid_brake_revision.load() == 1, "invalid episode did not bump the fault revision");`，并在已有"过期丢帧"用例中追加 `require(state.invalid_brake_revision.load() == 0, "stale drops bumped the fault revision");`（变量名按用例实际名称）。
  - `ice_vehicle_callbacks.test.py` 追加 offer 契约检查：

```python
    offer = section(source, 'state->pending.emplace_back("webrtc_offer", Json{', '});')
    for key in ('"transient_hold_capability"', '"transient_hold"', '"isolated_mock_bench_capability"'):
        if key not in offer:
            raise AssertionError(f"vehicle offer is missing {key}")
```

- [ ] **Step 2：运行，确认失败**

- [ ] **Step 3：增加故障类修订号**

`EncodedSendFreshness` 中 `key_unit_request_failures` 之后加入 `std::atomic<std::uint64_t> invalid_brake_revision{0};`；在 `encoded_send_freshness_probe` 的非法分支（`state.invalid_fault_event_latched = true;` 所在的 `if` 块内）追加 `++state.invalid_brake_revision;`。

- [ ] **Step 4：offer 声明能力**

在 offer 负载中加入：

```cpp
         {"transient_hold_capability", self->config.control.transient_hold.enabled ? detail::kTransientHoldCapability : std::string_view{}},
         {"transient_hold", self->config.control.transient_hold.enabled
             ? Json{{"control_tolerance_ms", self->config.control.degraded_timeout_ms + self->config.control.transient_hold.max_hold_ms},
                    {"video_tolerance_ms", self->config.runtime.critical_camera_stale_brake_ms + self->config.control.transient_hold.max_hold_ms},
                    {"max_hold_ms", self->config.control.transient_hold.max_hold_ms}}
             : Json(nullptr)},
```

并 include `mine_teleop/detail/transient_hold.hpp`。

- [ ] **Step 5：`enforce_control_recovery` 分类车端视频**

在 `Impl` 中新增成员 `bool vehicle_video_hold_active{false};`（只在 `control_recovery_phase_mutex` 下访问）。把逐路检查的循环替换为：

```cpp
    bool hold_mode = false;
    {
      std::lock_guard lock(control_mutex);
      hold_mode = control_service_started && control_service && control_service->transient_hold_enabled();
    }
    bool vehicle_video_stale = false;
    for (const auto& lane : lanes) {
      if (!lane->camera.critical_for_control) continue;
      if (lane->send_freshness) lane->send_freshness->max_age_ms = stale_brake_ms;
      const bool awaiting_recovery = (was_waiting || vehicle_video_hold_active) && lane->send_freshness &&
          lane->send_freshness->fresh_frames.load() < lane->video_wait_encoded_baseline.load() + 3;
      const auto state = detail::classify_vehicle_video_lane(lane->disabled.load(), lane->camera_faulted.load(),
          lane->send_freshness != nullptr, lane->send_freshness && lane->send_freshness->invalid_sample_blocked.load(),
          lane->send_freshness && camera_encoded_frame_fresh(lane->send_freshness->last_fresh_frame_ms.load(), now_ms, stale_brake_ms),
          lane->send_freshness ? lane->send_freshness->consecutive_anomalies.load() : 0U, awaiting_recovery);
      if (state == detail::VehicleVideoLaneState::Fault ||
          (state == detail::VehicleVideoLaneState::Stale && !hold_mode)) {
        ready = false;
        failed_lane = lane.get();
        break;
      }
      if (state == detail::VehicleVideoLaneState::Stale) vehicle_video_stale = true;
    }
    if (hold_mode && vehicle_video_stale && !vehicle_video_hold_active)
      for (const auto& lane : lanes)
        lane->video_wait_encoded_baseline = lane->send_freshness ? lane->send_freshness->fresh_frames.load() : 0;
    vehicle_video_hold_active = hold_mode && vehicle_video_stale;
```

协商生效时，喂给闩锁的车端视频修订号改用故障类修订号：把构造 `revisions` 的循环改为

```cpp
    for (const auto& lane : lanes) {
      if (!lane->camera.critical_for_control || !lane->send_freshness) continue;
      if (hold_mode) {
        const auto fault = lane->send_freshness->invalid_brake_revision.load();
        revisions.push_back({"video:" + lane->camera.id, fault, fault});
      } else {
        revisions.push_back({"video:" + lane->camera.id,
            lane->send_freshness->bad_sample_revision.load(), lane->send_freshness->brake_sample_revision.load()});
      }
    }
```

（这个循环要移到 `hold_mode` 计算之后。）

在 `control_mutex` 保护的块内，把 `ready = ready && control_service->driver_critical_video_ready(); if (!control_service->driver_critical_video_ready()) cause = ...` 改为：

```cpp
        const bool driver_video_fault = hold_mode ? control_service->driver_critical_video_fault()
                                                  : !control_service->driver_critical_video_ready();
        ready = ready && !driver_video_fault;
        if (driver_video_fault) cause = ControlRecoveryCause::CriticalVideo;
        control_service->observe_vehicle_video_transient(vehicle_video_hold_active, signaling.now_ms());
        transient_events = control_service->drain_transient_hold_events();
```

在该块前声明 `std::vector<Json> transient_events;`；在函数末尾（`apply_control_recovery_phase(...)` 之后）加入：

```cpp
    for (auto& event : transient_events) {
      const auto name = event.value("event", std::string("vehicle_transient_hold"));
      emit_diagnostic(name, name, "transient_hold", "", "No action is required unless the hold escalates.", true,
          std::move(event));
    }
```

- [ ] **Step 6：代际变化与硬停车诊断**
  - `handle_control_message` 中 `if (decision.changed) {` 块里，在 `control_service->set_control_braking_wait(true, ...)` 之前加入 `control_service->note_transport_epoch_changed(signaling.now_ms());`。
  - `control_hard_stop_transition` trace（约 3833 行）的记录对象中加入 `{"transient_hold", control_service ? control_service->control_timing(timestamp_ms).at("transient_hold") : Json(nullptr)}`，使硬停车诊断附带最近一次保持的信息（该处已持有 `control_mutex`）。
  - `ice_vehicle_callbacks.test.py` 追加检查：

```python
    epoch = section(source, "        if (decision.changed) {", "        }\n")
    if "note_transport_epoch_changed" not in epoch:
        raise AssertionError("transport epoch changes must escalate an active hold with their own reason")
```

- [ ] **Step 7：运行**

Run: Task 0 Step 2（包含 `mine-teleop-ice-vehicle-callback-mock-tests` 和 `mine-teleop-media-freshness-gstreamer-tests`）。
Expected：全部通过。

- [ ] **Step 8：提交**

```bash
git add cpp/include/mine_teleop/detail/gstreamer_freshness.hpp cpp/src/webrtc_media.cpp cpp/tests/media_freshness_gstreamer_tests.cpp cpp/tests/ice_vehicle_callbacks.test.py
git commit -m "Advertise transient hold and classify vehicle video stalls" -m "Co-Authored-By: Claude <noreply@anthropic.com>"
```

## Task 17：驾驶端协商与命令字段

**Files:**
- Modify: `cpp/include/mine_teleop/core.hpp`（`struct NativeControlIntent`）、`cpp/src/core.cpp`（`NativeControlIntent::validate`）
- Modify: `cpp/include/mine_teleop/server.hpp`（`DriverConsoleRuntime` 成员）、`cpp/src/server.cpp`（`poll_signaling`、`reset_native_control_state`、`send_native_control_sample`、`update_control_intent`、`status`）
- Test: `cpp/tests/control_transport_tests.cpp`

**Interfaces:**
- Consumes: Task 12 的 `parse_transient_hold_offer`；Task 14 的命令字段。
- Produces: `NativeControlIntent::critical_video_transient`；`DriverConsoleRuntime` 成员 `transient_hold_vehicle_capable_`、`transient_control_tolerance_ms_`、`transient_video_tolerance_ms_`（受 `mutex_` 保护）；`/api/status` 的 `native_control.transient_hold` 对象（字段：`negotiated`、`control_tolerance_ms`、`video_tolerance_ms`、`ack_rtt_anomaly_active`、`ack_rtt_anomaly_ms`、`ack_stall_ms`）。

- [ ] **Step 1：写失败的测试**（`control_transport_tests.cpp`；该文件第 12 行已定义 `struct DriverConsoleTransportTestAccess`，它是 `DriverConsoleRuntime` 的友元，在其中加入下面两个访问器）

```cpp
void driver_negotiates_transient_hold_from_offer() {
  DriverConfig config; config.driver_id = "driver-001"; config.signaling_url = "http://127.0.0.1:1";
  config.browser_event_log_path.clear();
  DriverConsoleRuntime runtime(config, "vehicle-001", "fixture");
  NativeControlIntent intent; intent.ui_instance_id = "ui"; intent.intent_seq = 1;
  intent.critical_video_ready = true; intent.critical_video_transient = true;
  bool rejected = false; try { intent.validate(); } catch (const std::invalid_argument&) { rejected = true; }
  expect(rejected, "intent accepted transient video while video was ready");
  const Json offer = {{"transient_hold_capability", "transient-hold-v1"},
      {"transient_hold", {{"control_tolerance_ms", 1000}, {"video_tolerance_ms", 1200}, {"max_hold_ms", 700}}}};
  DriverConsoleTransportTestAccess::observe_offer_capabilities(runtime, offer);
  expect(DriverConsoleTransportTestAccess::transient_tolerance(runtime) == 1000, "valid capability not negotiated");
  DriverConsoleTransportTestAccess::observe_offer_capabilities(runtime, Json::object());
  expect(DriverConsoleTransportTestAccess::transient_tolerance(runtime) == 0, "missing capability kept a stale negotiation");
}
```

`DriverConsoleTransportTestAccess` 中补：

```cpp
  static void observe_offer_capabilities(DriverConsoleRuntime& runtime, const Json& payload) {
    std::lock_guard lock(runtime.mutex_);
    runtime.observe_transient_hold_offer_locked(payload);
  }
  static int transient_tolerance(DriverConsoleRuntime& runtime) {
    std::lock_guard lock(runtime.mutex_);
    return runtime.transient_hold_vehicle_capable_ ? runtime.transient_control_tolerance_ms_ : 0;
  }
```

- [ ] **Step 2：运行，确认失败**（成员与方法不存在，编译失败）

- [ ] **Step 3：意图字段**
  - `NativeControlIntent` 在 `critical_video_brake_revision` 之后增加 `bool critical_video_transient{false};`。
  - `NativeControlIntent::validate()` 末尾加入 `if (critical_video_transient && critical_video_ready) throw std::invalid_argument("critical_video_transient requires critical_video_ready=false");`。
  - `update_control_intent` 在读取 `critical_video_sample_healthy` 之后加入：

```cpp
    if (input.contains("critical_video_transient")) {
      if (!input.at("critical_video_transient").is_boolean())
        throw std::invalid_argument("critical_video_transient must be a boolean");
      intent.critical_video_transient = !intent.estop && input.at("critical_video_transient").get<bool>();
    }
```

- [ ] **Step 4：能力解析**
  - `server.hpp` 在 `feedback_transport_vehicle_capable_` 附近加入：

```cpp
  bool transient_hold_vehicle_capable_{false};  // mutex_
  int transient_control_tolerance_ms_{0};       // mutex_
  int transient_video_tolerance_ms_{0};         // mutex_
  void observe_transient_hold_offer_locked(const Json& payload);
```

  - `server.cpp` include `mine_teleop/detail/transient_hold.hpp`，实现：

```cpp
void DriverConsoleRuntime::observe_transient_hold_offer_locked(const Json& payload) {
  const auto offer = detail::parse_transient_hold_offer(payload);
  transient_hold_vehicle_capable_ = offer.has_value();
  transient_control_tolerance_ms_ = offer ? offer->control_tolerance_ms : 0;
  transient_video_tolerance_ms_ = offer ? offer->video_tolerance_ms : 0;
}
```

  - `poll_signaling` 的 offer 解析处（`feedback_transport_vehicle_capable_ = ...` 之后）加入 `observe_transient_hold_offer_locked(payload);`。
  - `DriverConfig` 增加进程内开关（不从 YAML 读取）`bool disable_transient_hold{false};`，并让 `observe_transient_hold_offer_locked` 在开关为真时直接按"不支持"处理：
    `const auto offer = config_.disable_transient_hold ? std::nullopt : detail::parse_transient_hold_offer(payload);`。
    Windows 原生客户端在设置 `config.receive_only` 的同一处加入 `config.disable_transient_hold = true;`（设计 8.4：mock 客户端保持现有行为）。
    在 Step 1 的测试中补一条：`disable_transient_hold = true` 的运行时收到有效 offer 后，`transient_tolerance(runtime) == 0`。
  - `reset_native_control_state` 在 `std::lock_guard lock(mutex_);` 块内加入 `observe_transient_hold_offer_locked(Json::object());`。
- [ ] **Step 5：命令负载**

`send_native_control_sample` 中：
  - 在 `mutex_` 快照块（`vehicle_transport_capable = ...` 附近）加入 `transient_tolerance_ms = transient_hold_vehicle_capable_ ? transient_control_tolerance_ms_ : 0;`，并在函数开头局部变量区声明 `int transient_tolerance_ms = 0;`。
  - 在 `command.critical_video_ready = sample.intent.critical_video_ready;` 之后加入：

```cpp
    command.transient_hold_capable = transient_tolerance_ms > 0;
    command.critical_video_transient = command.transient_hold_capable && !command.critical_video_ready &&
        sample.intent.critical_video_transient;
```

  （`payload.update(command.to_json())` 会把两个字段带进签名负载，无需另写。）
- [ ] **Step 6：状态输出**

`status()` 的 `native_control` 对象中加入（ACK 相关字段在 Task 18 接入真实值，本任务先输出协商结果和容忍上限）：

```cpp
        {"transient_hold", {{"negotiated", transient_hold_negotiated},
                            {"control_tolerance_ms", transient_control_tolerance},
                            {"video_tolerance_ms", transient_video_tolerance}}},
```

其中三个局部变量在 `status()` 已有的 `std::lock_guard state_lock(mutex_)` 块内读取。

- [ ] **Step 7：运行**

Run: Task 0 Step 2。
Expected：新用例通过；现有驾驶端测试不回归（未协商时命令 JSON 不变）。

- [ ] **Step 8：提交**

```bash
git add cpp/include/mine_teleop/core.hpp cpp/src/core.cpp cpp/include/mine_teleop/server.hpp cpp/src/server.cpp cpp/tests/control_transport_tests.cpp
git commit -m "Negotiate transient hold on the native driver" -m "Co-Authored-By: Claude <noreply@anthropic.com>"
```

## Task 18：ACK 判据容忍模式与 DC 切换规则

**Files:**
- Modify: `cpp/include/mine_teleop/detail/native_control_acknowledgement_window.hpp`（`class NativeControlAckRecovery`）
- Modify: `cpp/src/server.cpp`（`send_native_control_sample` 设置容忍上限；`status()` 输出 ACK 字段）
- Test: `cpp/tests/native_control_ack_tests.cpp`

**Interfaces:**
- Produces: `void set_tolerance_ms(int tolerance_ms)`（0 为现有规则）、`int tolerance_ms() const`、`std::optional<std::int64_t> rtt_anomaly_since() const`。

- [ ] **Step 1：写失败的测试**（`native_control_ack_tests.cpp`，加入并在 `main()` 调用）

```cpp
void tolerant_mode_absorbs_short_rtt_spikes() {
  NativeControlAckRecovery recovery; recovery.set_tolerance_ms(1000);
  std::uint64_t seq = 0;
  for (std::int64_t now = 0; now < 900; now += 50)
    expect(!recovery.note_ack_progress(now, ++seq, 300, 5000).entered, "short RTT spike entered the ACK wait");
  expect(!recovery.waiting() && recovery.bad_sample_revision() > 0, "spike was not recorded as link degradation");
  expect(!recovery.note_ack_progress(950, ++seq, 50, 5000).entered && !recovery.rtt_anomaly_since(), "healthy ACK did not clear the anomaly");
}
void tolerant_mode_escalates_sustained_rtt() {
  NativeControlAckRecovery recovery; recovery.set_tolerance_ms(1000);
  std::uint64_t seq = 0; bool entered = false; std::int64_t entered_at = -1;
  for (std::int64_t now = 0; now <= 1100 && !entered; now += 50) {
    entered = recovery.note_ack_progress(now, ++seq, 300, 5000).entered;
    if (entered) entered_at = now;
  }
  expect(entered && entered_at >= 1000, "sustained RTT anomaly did not enter the wait at the tolerance");
}
void tolerant_mode_stall_uses_tolerance() {
  NativeControlAckRecovery recovery; recovery.set_tolerance_ms(1000);
  static_cast<void>(recovery.note_ack_progress(0, 1, 50, 5000));
  expect(!recovery.observe(600, 2, 600, 1, 50, 5000).entered, "600 ms stall entered in tolerant mode");
  expect(recovery.observe(1000, 2, 1000, 1, 50, 5000).entered, "1000 ms stall did not enter");
}
void legacy_mode_unchanged() {
  NativeControlAckRecovery recovery;
  std::uint64_t seq = 0;
  static_cast<void>(recovery.note_ack_progress(0, ++seq, 300, 5000));
  static_cast<void>(recovery.note_ack_progress(50, ++seq, 300, 5000));
  expect(recovery.note_ack_progress(100, ++seq, 300, 5000).entered, "legacy third anomaly did not enter");
  NativeControlAckRecovery stall;
  static_cast<void>(stall.note_ack_progress(0, 1, 50, 5000));
  expect(stall.observe(500, 2, 500, 1, 50, 5000).entered, "legacy 500 ms stall did not enter");
}
```

- [ ] **Step 2：运行，确认失败**（`set_tolerance_ms` 不存在）

- [ ] **Step 3：实现容忍模式**

在类中加入公开方法与成员：

```cpp
  // 0 keeps the existing rule. A positive tolerance is the vehicle's declared
  // control tolerance: RTT-only anomalies and ACK stalls escalate only once
  // they have lasted that long; the vehicle owns shorter transients.
  void set_tolerance_ms(int tolerance_ms) {
    tolerance_ms_ = tolerance_ms > 0 ? tolerance_ms : 0;
    if (tolerance_ms_ == 0) anomaly_since_.reset();
  }
  [[nodiscard]] int tolerance_ms() const { return tolerance_ms_; }
  [[nodiscard]] std::optional<std::int64_t> rtt_anomaly_since() const { return anomaly_since_; }
```

私有区加 `int tolerance_ms_{0}; std::optional<std::int64_t> anomaly_since_;`，文件 include `<optional>`。

`note_ack_progress` 的异常分支改为：

```cpp
    if (ack_packet_age_ms > kFreshAckMs) {
      advance_revision(bad_sample_revision_);
      consecutive_anomalies_ = std::min(consecutive_anomalies_ + 1, kConsecutiveAnomalies);
      if (tolerance_ms_ > 0) {
        if (!anomaly_since_) anomaly_since_ = now;
        if (!waiting_ && now - *anomaly_since_ >= tolerance_ms_) { enter(now, timeout_ms, ack_seq); result.entered = true; }
      } else if (!waiting_ && consecutive_anomalies_ >= kConsecutiveAnomalies) {
        enter(now, timeout_ms, ack_seq);
        result.entered = true;
      }
    } else {
      consecutive_anomalies_ = 0;
      anomaly_since_.reset();
      if (waiting_) {
        waiting_ = false;
        result.recovered = true;
      }
    }
```

`observe()` 的停滞判断改为：

```cpp
    const int stall_threshold = tolerance_ms_ > 0 ? tolerance_ms_ : kStallMs;
    if (!waiting_ && pending > 0 && stall_ms >= stall_threshold) {
      enter(now, timeout_ms, ack_seq);
      result.entered = true;
    }
```

（`reconnect_due` 判断保持使用 `kStallMs`。）

- [ ] **Step 4：驾驶端每次采样同步容忍上限**

`send_native_control_sample` 在持有 `control_signaling_websocket_mutex_` 的块里、`control_signaling_ack_recovery_.observe(...)` 之前加入：

```cpp
      control_signaling_ack_recovery_.set_tolerance_ms(transient_tolerance_ms);
      control_dc_ack_recovery_.set_tolerance_ms(transient_tolerance_ms);
```

（DC 结果在 courier 线程上经 `consume_vehicle_control_result_locked` 调用 `note_ack_progress`，同样持有这把锁；DC 只在 `transition.entered` 时切换到 WSS，因此容忍模式下只有持续异常才会切换，不需要另改。）

在 `control_signaling_ack_recovery_.observe(...)` 之后记录往返异常的开始与结束（`server.hpp` 新增成员 `bool ack_rtt_anomaly_traced_{false};`，受同一把 WSS 锁保护；`close_control_signaling_websocket` 中复位为 false）：

```cpp
      const bool rtt_anomaly = control_signaling_ack_recovery_.rtt_anomaly_since().has_value();
      if (rtt_anomaly != ack_rtt_anomaly_traced_) {
        ack_rtt_anomaly_traced_ = rtt_anomaly;
        if (native_control_trace_) native_control_trace_->enqueue({
            {"stage", rtt_anomaly ? "ack_rtt_anomaly_started" : "ack_rtt_anomaly_ended"},
            {"trace_session_id", session}, {"seq", sequence}, {"event_monotonic_ms", monotonic_ms},
            {"tolerance_ms", transient_tolerance_ms}});
      }
```

`status()` 的 `transient_hold` 对象补充（在已持有 `control_signaling_websocket_mutex_` 的位置读取）：

```cpp
  {"ack_rtt_anomaly_active", control_signaling_ack_recovery_.rtt_anomaly_since().has_value()},
  {"ack_rtt_anomaly_ms", control_signaling_ack_recovery_.rtt_anomaly_since()
      ? Json(monotonic_now_ms() - *control_signaling_ack_recovery_.rtt_anomaly_since()) : Json(nullptr)},
  {"ack_stall_ms", control_signaling_ack_window_.stall_age_ms(monotonic_now_ms())},
```

- [ ] **Step 5：运行**

Run: Task 0 Step 2。
Expected：`mine-teleop-native-control-ack-tests` 全部通过；`control_transport_tests` 中的 `late_dc_results_withdraw_publication_in_result_handler`（未协商）仍通过。

- [ ] **Step 6：提交**

```bash
git add cpp/include/mine_teleop/detail/native_control_acknowledgement_window.hpp cpp/src/server.cpp cpp/tests/native_control_ack_tests.cpp
git commit -m "Tolerate short ACK round-trip spikes when the vehicle holds" -m "Co-Authored-By: Claude <noreply@anthropic.com>"
```

## Task 19：页面——视频瞬态分类、保持提示、不清输入

**Files:**
- Modify: `cpp/web/camera_view.js`（导出 `criticalVideoTransient`；`observeBadSamples`、`observeCriticalTrack`、`criticalHealth`；挂载对象增加 `setTransientHoldNegotiated`）
- Modify: `cpp/web/control_logic.js`（`controlRecoveryGuidance` 新增 `transient_hold` 阶段）
- Modify: `cpp/src/server.cpp`（内嵌页面：意图字段、协商状态传给画面、保持事件日志、监控面板）
- Test: `cpp/tests/camera_view.test.cjs`、`cpp/tests/control_logic.test.cjs`

- [ ] **Step 1：写失败的 JS 测试**

`camera_view.test.cjs`（修改顶部 require，加入 `criticalVideoTransient`）：

```js
test('only stale/recovering/muted critical views are transient', () => {
  assert.equal(criticalVideoTransient([{ready: false, reason: 'presented_frame_stale'}, {ready: true, reason: 'ready'}], false), true);
  assert.equal(criticalVideoTransient([{ready: false, reason: 'track_muted'}, {ready: false, reason: 'recovering_frames'}], false), true);
  assert.equal(criticalVideoTransient([{ready: false, reason: 'track_ended'}], false), false);
  assert.equal(criticalVideoTransient([{ready: false, reason: 'presented_frame_stale'}, {ready: false, reason: 'peer_disconnected'}], false), false);
  assert.equal(criticalVideoTransient([{ready: false, reason: 'presented_frame_stale'}], true), false);
  assert.equal(criticalVideoTransient([{ready: true, reason: 'ready'}], false), false);
});
```

`control_logic.test.cjs`：

```js
test('transient hold guidance keeps input and does not ask for a re-press', () => {
  const timing = {control_braking_wait: false, recovery_brake_hold: false, rearm_required: false,
    transient_hold: {active: true, causes: ['control_gap'], elapsed_ms: 180, max_hold_ms: 700}};
  const guidance = logic.controlRecoveryGuidance(timing, true);
  assert.equal(guidance.visible, true);
  assert.equal(guidance.phase, 'transient_hold');
  assert.match(guidance.text, /已暂停牵引（轻制动）/);
  assert.match(guidance.text, /180\/700 ms/);
  assert.doesNotMatch(guidance.text, /重新按下/);
  assert.equal(logic.degradedInputRearmRequired(timing, 0), false);
});
test('escalated braking wait keeps the existing re-press guidance', () => {
  const guidance = logic.controlRecoveryGuidance({control_braking_wait: true, rearm_required: true,
    driver_control_link_ready: false, transient_hold: {active: false}}, true);
  assert.notEqual(guidance.phase, 'transient_hold');
});
test('a video-only hold shows an info banner while the vehicle stays CONTROL_ACTIVE', () => {
  const telemetry = {safety_state: 'CONTROL_ACTIVE', estop: false, control_timing: {control_braking_wait: false,
    recovery_brake_hold: false, rearm_required: false,
    transient_hold: {active: true, causes: ['vehicle_video_stale'], elapsed_ms: 90, max_hold_ms: 700}}};
  const view = logic.deriveControlRecoveryPresentation(true, telemetry, true, false);
  assert.equal(view.visible, true);
  assert.equal(view.severity, 'info');
  assert.equal(view.cause, 'transient_hold');
  assert.doesNotMatch(view.banner, /重新按下/);
});
test('budget escalation explains the unstable link', () => {
  const guidance = logic.controlRecoveryGuidance({control_braking_wait: true, rearm_required: true,
    driver_control_link_ready: true, recovery_samples_healthy: false,
    transient_hold: {active: false, holds_in_window: 7,
      last: {outcome: 'escalated', escalation_reason: 'budget_count_exceeded'}}}, true);
  assert.match(guidance.text, /链路不稳定（1 分钟内保持 7 次）/);
});
test('control page reports transient video and negotiation to the camera view', () => {
  assert.match(controlPageSource, /critical_video_transient:viewHealth\.ready!==true&&viewHealth\.transient===true/);
  assert.match(controlPageSource, /cameraView\.setTransientHoldNegotiated\(/);
  assert.match(controlPageSource, /driver_transient_hold_observed/);
});
```

- [ ] **Step 2：运行，确认失败**

Run: `node cpp/tests/camera_view.test.cjs && node cpp/tests/control_logic.test.cjs`
Expected：`criticalVideoTransient is not a function`、`phase` 不是 `transient_hold` 等失败。

- [ ] **Step 3：实现 `camera_view.js`**
  - 工厂函数内、`frameHealth` 附近加入并加进导出的 `Object.freeze({...})`：

```js
  // Transient means every blocked critical view is only stale, recovering or
  // muted. Ended/missing/paused/disconnected views or revision overflow are faults.
  const TRANSIENT_VIDEO_REASONS = new Set(['presented_frame_stale', 'recovering_frames', 'awaiting_valid_frame', 'track_muted']);
  function criticalVideoTransient(cameras, overflow) {
    if (overflow || !Array.isArray(cameras)) return false;
    const blocked = cameras.filter(camera => camera && camera.ready !== true);
    return blocked.length > 0 && blocked.every(camera => TRANSIENT_VIDEO_REASONS.has(camera.reason));
  }
```

  - `mount()` 内加入 `let transientHoldNegotiated = false;` 和 `function setTransientHoldNegotiated(value) { transientHoldNegotiated = value === true; }`，并加进挂载返回对象 `{configure, attach, ..., presentationSnapshot, setTransientHoldNegotiated}`。
  - `observeBadSamples(observation, count)` 改为：

```js
    function observeBadSamples(observation, count, transient = false) {
      advanceRevision(observation.history, 'bad', count);
      const previous = observation.consecutiveAnomalies;
      observation.consecutiveAnomalies = Math.min(3, previous + count);
      // Negotiated transient anomalies stay visible in the bad revision only;
      // the vehicle holds traction instead of entering the braking wait.
      if (previous < 3 && observation.consecutiveAnomalies === 3 && !(transient && transientHoldNegotiated))
        advanceRevision(observation.history, 'brake');
    }
```

  - `observeCriticalTrack` 中在 `stateHealthy` 之后计算 `const transientState = Boolean(track && track.readyState === 'live' && track.muted && !disconnected && videoUsable);`，两处 `observeBadSamples(observation, ...)` 都传入第三个参数 `transientState`。
  - `criticalHealth` 返回对象增加 `transient: criticalVideoTransient(cameras, revision.overflow)`。
- [ ] **Step 4：实现 `control_logic.js` 的保持阶段**

在 `controlRecoveryGuidance` 中、`if (!waiting && !holding && !rearm) return hidden;` 之前加入：

```js
    const hold = timing.transient_hold && typeof timing.transient_hold === 'object' ? timing.transient_hold : null;
    if (feedbackFresh && hold && hold.active === true && !waiting) {
      const labels = {control_gap: '控制链路中断', late_commands: '控制命令迟到',
        driver_video_stale: '驾驶端画面停顿', vehicle_video_stale: '车端画面停顿'};
      const causes = Array.isArray(hold.causes) ? hold.causes.filter(cause => labels[cause]) : [];
      const elapsed = Number.isFinite(hold.elapsed_ms) ? Math.max(0, Math.round(hold.elapsed_ms)) : null;
      const limit = Number.isFinite(hold.max_hold_ms) ? hold.max_hold_ms : null;
      const reason = causes.length ? causes.map(cause => labels[cause]).join('、') : '链路或画面短暂波动';
      const progress = elapsed !== null && limit ? `（${elapsed}/${limit} ms）` : '';
      return {visible: true, phase: 'transient_hold', causes, reason, nextStep: '无需操作，恢复后自动继续',
        text: `链路/画面短暂波动，已暂停牵引（轻制动）${progress} · 原因：${reason} · 无需操作，恢复后自动继续`};
    }
```

同一函数中，在最终 `return {visible: true, phase, causes, reason, nextStep, text: ...}` 处加入频次预算升级的说明：

```js
    const holdLast = hold && hold.last && typeof hold.last === 'object' ? hold.last : null;
    const unstable = waiting && holdLast && holdLast.outcome === 'escalated' &&
      ['budget_count_exceeded', 'budget_time_exceeded'].includes(holdLast.escalation_reason);
    const holds = Number.isInteger(hold && hold.holds_in_window) ? hold.holds_in_window : '多';
    const prefix = unstable ? `链路不稳定（1 分钟内保持 ${holds} 次）· ` : '';
    return {visible: true, phase, causes, reason, nextStep, text: `${prefix}当前阻挡：${reason} · 下一步：${nextStep}`};
```

`deriveControlRecoveryPresentation`（约 1122–1145 行）改为同时覆盖保持状态：把 `if (!waiting && !holding && !delayed) return hidden;` 改为

```js
    const transientHold = Boolean(timing.transient_hold && timing.transient_hold.active === true);
    if (!waiting && !holding && !delayed && !transientHold) return hidden;
```

并在 `const guidance = controlRecoveryGuidance(timing, feedbackFresh);` 之后加入：

```js
    if (feedbackFresh && guidance.visible && guidance.phase === 'transient_hold')
      return {visible: true, severity: 'info', banner: guidance.text, alert: guidance.text, cause: 'transient_hold'};
```

（`deriveControlRecoveryPresentation` 已在 `control_logic.js` 的导出对象中，第 1790 行。）

`camera_view.js` 的 `paint()` 中把 `stale: '画面中断 / 暂无新帧'` 改为 `stale: '画面短暂中断'`（`offline: '视频已断开'` 不变），并同步更新 `camera_view.test.cjs` 中断言旧文案的用例（`grep -n "画面中断" cpp/tests/*.cjs` 找出）。
- [ ] **Step 5：实现内嵌页面**（`server.cpp`）
  - 意图 `normalized` 对象中、`critical_video_sample_healthy` 之后加入 `critical_video_transient:viewHealth.ready!==true&&viewHealth.transient===true`。
  - 每次更新 `latestRuntimeStatus`（`/api/status` 轮询结果赋值处）之后加入：
    `cameraView.setTransientHoldNegotiated(latestRuntimeStatus?.native_control?.transient_hold?.negotiated===true);`
  - 处理车端遥测、调用 `applyVehicleSafetyState(...)` 的位置旁边加入状态变化日志：

```js
const transientHoldActive=timing?.transient_hold?.active===true;
if(transientHoldActive!==lastTransientHoldActive){clientLog('driver_transient_hold_observed',{active:transientHoldActive,causes:timing?.transient_hold?.causes||[],last:timing?.transient_hold?.last||null});lastTransientHoldActive=transientHoldActive}
```

    并在页面顶层变量声明处加 `let lastTransientHoldActive=false;`。
  - 监控面板（`renderMonitoring` 中现有控制时序显示处）追加一行：最近 1 分钟保持次数 `timing.transient_hold.holds_in_window`、最近一次保持时长 `timing.transient_hold.last?.duration_ms`、结果 `timing.transient_hold.last?.outcome`。
- [ ] **Step 6：运行**

Run: `python3 scripts/test/run_offline_regressions.py`（宿主机）。
Expected：全部 `[PASS]`。

- [ ] **Step 7：提交**

```bash
git add cpp/web/camera_view.js cpp/web/control_logic.js cpp/src/server.cpp cpp/tests/camera_view.test.cjs cpp/tests/control_logic.test.cjs
git commit -m "Report transient video stalls and show the hold state" -m "Co-Authored-By: Claude <noreply@anthropic.com>"
```

## Task 20：集成测试——真实回环链路上的尖峰

**Files:**
- Modify: `cpp/tests/control_transport_integration_tests.cpp`（夹具增加 `transient` 参数；新增两个场景；新增 `--transient-hold-only`）
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: Task 7 后的 `Fixture(bool direct, bool feedback, bool product, std::string mode, bool defer_owner)`。
- Produces: `Fixture(bool direct = false, bool feedback = false, bool product = false, std::string mode = "p2p_preferred", bool defer_owner = false, bool transient = false)`。

- [ ] **Step 1：夹具支持瞬态保持**
  - 构造函数末尾增加参数 `bool transient = false`，保存为成员 `transient_hold`。
  - offer 负载中加入：

```cpp
                     {"transient_hold_capability", transient_hold ? kTransientHoldCapability : std::string_view{}},
                     {"transient_hold", transient_hold ? Json{{"control_tolerance_ms", 1000}, {"video_tolerance_ms", 1200}, {"max_hold_ms", 700}} : Json(nullptr)},
```

  - 创建车端服务前：`if (transient_hold) { config_vehicle.control.control_timeout_ms = 1200; config_vehicle.control.transient_hold.enabled = true; }`。
  - 文件 include `mine_teleop/detail/transient_hold.hpp`（文件第 77 行已有 `using namespace mine_teleop::detail;`，可直接使用 `kTransientHoldCapability`）。
- [ ] **Step 2：写场景**

```cpp
Json vehicle_timing(Fixture& fixture) {
  std::lock_guard lock(fixture.car_mutex);
  return fixture.vehicle->control_timing(utc());
}
void drive_until_motion(Fixture& fixture) {
  fixture.requested_throttle = 0.4;
  const auto deadline = steady() + 8000;
  while (steady() < deadline) {
    fixture.pump(false);
    const auto timing = vehicle_timing(fixture);
    if (timing.at("transient_hold").value("enabled", false) && !timing.value("control_braking_wait", true) &&
        fixture.last_result_reason == "accepted") return;
  }
  throw std::runtime_error("transient fixture never reached negotiated motion");
}
void cut_vehicle_delivery(Fixture& fixture, std::int64_t duration_ms, bool& saw_hold, bool& saw_wait) {
  fixture.cloud_online = false;
  const auto started = steady();
  while (steady() - started < duration_ms) {
    fixture.pump(false);
    const auto timing = vehicle_timing(fixture);
    saw_hold = saw_hold || timing.at("transient_hold").value("active", false);
    saw_wait = saw_wait || timing.value("control_braking_wait", false);
  }
  fixture.cloud_online = true;
}
void transient_hold_absorbs_short_spikes_and_escalates_long_outage() {
  Fixture fixture(false, false, false, "cloud_only", false, true);
  drive_until_motion(fixture);
  bool saw_hold = false, saw_wait = false;
  cut_vehicle_delivery(fixture, 450, saw_hold, saw_wait);
  const auto resume_deadline = steady() + 1500;
  while (steady() < resume_deadline) {
    fixture.pump(false);
    const auto timing = vehicle_timing(fixture);
    saw_wait = saw_wait || timing.value("control_braking_wait", false);
    if (timing.at("transient_hold").at("last").is_object() &&
        timing.at("transient_hold").at("last").value("outcome", "") == "resumed") break;
  }
  const auto after_short = vehicle_timing(fixture);
  expect(saw_hold && !saw_wait, "450 ms delivery gap did not hold without braking wait: " + after_short.dump());
  expect(after_short.at("transient_hold").at("last").value("outcome", "") == "resumed",
      "short spike did not resume automatically: " + after_short.dump());
  expect(!fixture.driver->status().at("native_control").value("intent_requires_fresh_input", false),
      "short spike invalidated the held driver input");
  saw_hold = saw_wait = false;
  cut_vehicle_delivery(fixture, 1150, saw_hold, saw_wait);
  expect(saw_wait, "1150 ms delivery gap did not escalate to braking wait");
  expect(vehicle_timing(fixture).at("transient_hold").at("last").value("escalation_reason", "") == "hold_duration_exceeded",
      "long outage escalated for the wrong reason");
}
void transient_hold_budget_escalates_repeated_spikes() {
  Fixture fixture(false, false, false, "cloud_only", false, true);
  drive_until_motion(fixture);
  bool escalated = false;
  for (int spike = 0; spike < 8 && !escalated; ++spike) {
    bool saw_hold = false, saw_wait = false;
    cut_vehicle_delivery(fixture, 420, saw_hold, saw_wait);
    const auto settle = steady() + 600;
    while (steady() < settle) { fixture.pump(false); saw_wait = saw_wait || vehicle_timing(fixture).value("control_braking_wait", false); }
    escalated = saw_wait;
  }
  expect(escalated && vehicle_timing(fixture).at("transient_hold").at("last").value("escalation_reason", "") == "budget_count_exceeded",
      "repeated spikes did not exhaust the hold budget");
}
```

（`fixture.last_result_reason`、`requested_throttle`、`car_mutex`、`cloud_online` 都是夹具已有成员；夹具 `apply()` 对已接受命令写入的原因是车端服务返回的 `"accepted"`。）
- [ ] **Step 3：注册运行模式**

`main()` 的分支中加入：`if (argc > 1 && std::string_view(argv[1]) == "--transient-hold-only") { transient_hold_absorbs_short_spikes_and_escalates_long_outage(); transient_hold_budget_escalates_repeated_spikes(); return 0; }`

`CMakeLists.txt`：

```cmake
  add_test(NAME mine-teleop-transient-hold-integration-tests
    COMMAND mine-teleop-control-transport-integration-tests --transient-hold-only)
  set_tests_properties(mine-teleop-transient-hold-integration-tests PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 60)
```

- [ ] **Step 4：运行**

Run: Task 0 Step 2；再单独重复 5 次 `ctest --test-dir /build -R transient-hold-integration --repeat until-fail:5`。
Expected：5 次全部通过；如出现偶发失败，按 systematic-debugging 流程找根因，不得用重试掩盖。

- [ ] **Step 5：提交**

```bash
git add cpp/tests/control_transport_integration_tests.cpp CMakeLists.txt
git commit -m "Exercise transient hold over the real loopback control path" -m "Co-Authored-By: Claude <noreply@anthropic.com>"
```

## Task 21：网络层尖峰注入（Linux，tc netem）

**Files:**
- Modify: `cpp/tests/control_transport_integration_tests.cpp`（新增 `--transient-hold-netem`）
- Modify: `CMakeLists.txt`

- [ ] **Step 1：实现场景**（只在 Linux 编译；其他平台该模式直接返回 77）

```cpp
#if defined(__linux__)
#include <unistd.h>
int netem(const std::string& arguments) {
  return std::system(("tc qdisc " + arguments + " >/dev/null 2>&1").c_str());
}
int transient_hold_survives_real_loopback_loss() {
  if (geteuid() != 0 || netem("replace dev lo root netem loss 0%") != 0) return 77;
  struct Restore { ~Restore() { static_cast<void>(netem("del dev lo root")); } } restore;
  Fixture fixture(false, false, false, "cloud_only", false, true);
  drive_until_motion(fixture);
  static_cast<void>(netem("del dev lo root"));
  // 350 ms of real loss: TCP retransmission adds head-of-line delay on recovery.
  bool saw_wait = false;
  expect(netem("replace dev lo root netem loss 100%") == 0, "netem loss could not be applied");
  const auto lossy = steady();
  while (steady() - lossy < 350) { fixture.pump(false); saw_wait = saw_wait || vehicle_timing(fixture).value("control_braking_wait", false); }
  static_cast<void>(netem("del dev lo root"));
  const auto settle = steady() + 2000;
  while (steady() < settle) { fixture.pump(false); saw_wait = saw_wait || vehicle_timing(fixture).value("control_braking_wait", false); }
  expect(!saw_wait, "350 ms of real loopback loss escalated: " + vehicle_timing(fixture).dump());
  // 1500 ms of loss is not transient.
  expect(netem("replace dev lo root netem loss 100%") == 0, "netem loss could not be re-applied");
  const auto outage = steady();
  while (steady() - outage < 1500) { fixture.pump(false); saw_wait = saw_wait || vehicle_timing(fixture).value("control_braking_wait", false); }
  static_cast<void>(netem("del dev lo root"));
  expect(saw_wait, "1500 ms of real loopback loss did not escalate");
  return 0;
}
#else
int transient_hold_survives_real_loopback_loss() { return 77; }
#endif
```

`main()` 中：`if (argc > 1 && std::string_view(argv[1]) == "--transient-hold-netem") return transient_hold_survives_real_loopback_loss();`（文件顶部 include `<cstdlib>`）。

说明：丢包期间 `pump()` 的读取有 1 ms 超时、写入进入内核发送缓冲，不应阻塞；车端看门狗线程 `car_watchdog` 独立推进车端状态。在两个丢包循环里记录单次 `pump()` 的最大耗时并在失败信息中输出；若超过 100 ms，说明夹具被阻塞，测试结果无效，需要先修夹具再判断。

- [ ] **Step 2：注册**

```cmake
  add_test(NAME mine-teleop-transient-hold-netem-tests
    COMMAND mine-teleop-control-transport-integration-tests --transient-hold-netem)
  set_tests_properties(mine-teleop-transient-hold-netem-tests PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 60 SKIP_RETURN_CODE 77)
```

- [ ] **Step 3：运行**

Run: Task 0 Step 2（容器带 `NET_ADMIN`，以 root 运行）。
Expected：`mine-teleop-transient-hold-netem-tests` 为 PASS（不是 SKIP）。若显示 SKIP，检查容器内 `tc qdisc add dev lo root netem loss 0%` 的错误输出（常见原因：宿主内核缺 `sch_netem` 模块），并在汇报中如实列为"未执行"。

- [ ] **Step 4：提交**

```bash
git add cpp/tests/control_transport_integration_tests.cpp CMakeLists.txt
git commit -m "Inject real loopback loss into the transient hold path" -m "Co-Authored-By: Claude <noreply@anthropic.com>"
```

## Task 22：现场验收分析脚本与文档

**Files:**
- Create: `scripts/test/transient_hold_analyze.py`、`scripts/test/transient_hold_analyze.test.py`
- Modify: `scripts/test/run_offline_regressions.py`（`PURE_TESTS` 增加一项）
- Modify: `docs/64-transient-link-hold-design.md`（状态改为"已实现，待现场验收"）、`docs/07-control-and-safety.md`、`docs/08-configuration.md`、`docs/11-testing-and-validation.md`、`docs/DEPLOY.md`

- [ ] **Step 1：写失败的测试** `scripts/test/transient_hold_analyze.test.py`

```python
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).with_name("transient_hold_analyze.py")


class TransientHoldAnalyzeTest(unittest.TestCase):
    def run_script(self, lines):
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory) / "vehicle-runtime.log"
            log.write_text("\n".join(lines) + "\n", encoding="utf-8")
            output = subprocess.run([sys.executable, str(SCRIPT), str(log)], check=True, capture_output=True, text=True)
            return json.loads(output.stdout)

    def test_summarizes_holds_escalations_and_hard_stops(self):
        events = [
            {"event": "vehicle_transient_hold_entered", "causes": ["control_gap"]},
            {"event": "vehicle_transient_hold_resumed", "last": {"duration_ms": 120, "outcome": "resumed"}},
            {"event": "vehicle_transient_hold_entered", "causes": ["late_commands"]},
            {"event": "vehicle_transient_hold_escalated", "last": {"duration_ms": 700, "outcome": "escalated",
                                                                  "escalation_reason": "hold_duration_exceeded"}},
            {"event": "control_hard_stop_transition"},
        ]
        lines = [json.dumps(event) for event in events] + ["not json", ""]
        summary = self.run_script(lines)
        self.assertEqual(summary["holds_entered"], 2)
        self.assertEqual(summary["resumed"], 1)
        self.assertEqual(summary["escalated"], 1)
        self.assertEqual(summary["escalation_reasons"], {"hold_duration_exceeded": 1})
        self.assertEqual(summary["hard_stops"], 1)
        self.assertEqual(summary["resumed_duration_ms"]["max"], 120)
        self.assertEqual(summary["ignored_lines"], 1)


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 2：运行，确认失败**

Run: `python3 scripts/test/transient_hold_analyze.test.py`
Expected: FAIL（脚本不存在）。

- [ ] **Step 3：实现** `scripts/test/transient_hold_analyze.py`

```python
#!/usr/bin/env python3
"""Summarize transient hold events from exported vehicle runtime logs (JSONL).

Offline only: reads the given files, never contacts a vehicle or service.
"""
import argparse
import json
import sys
from collections import Counter
from pathlib import Path


def percentile(values, fraction):
    if not values:
        return None
    ordered = sorted(values)
    index = min(len(ordered) - 1, max(0, round(fraction * (len(ordered) - 1))))
    return ordered[index]


def summarize(paths):
    entered = resumed = escalated = hard_stops = ignored = 0
    reasons = Counter()
    causes = Counter()
    durations = []
    for path in paths:
        for line in Path(path).read_text(encoding="utf-8", errors="replace").splitlines():
            if not line.strip():
                continue
            try:
                event = json.loads(line)
            except json.JSONDecodeError:
                ignored += 1
                continue
            name = event.get("event") if isinstance(event, dict) else None
            last = event.get("last") if isinstance(event, dict) and isinstance(event.get("last"), dict) else {}
            if name == "vehicle_transient_hold_entered":
                entered += 1
                causes.update(cause for cause in event.get("causes", []) if isinstance(cause, str))
            elif name == "vehicle_transient_hold_resumed":
                resumed += 1
                if isinstance(last.get("duration_ms"), (int, float)):
                    durations.append(last["duration_ms"])
            elif name == "vehicle_transient_hold_escalated":
                escalated += 1
                reasons[last.get("escalation_reason") or "unknown"] += 1
            elif name == "control_hard_stop_transition":
                hard_stops += 1
    return {
        "holds_entered": entered, "resumed": resumed, "escalated": escalated, "hard_stops": hard_stops,
        "causes": dict(causes), "escalation_reasons": dict(reasons), "ignored_lines": ignored,
        "resumed_duration_ms": {"count": len(durations), "p50": percentile(durations, 0.5),
                                "p95": percentile(durations, 0.95), "max": max(durations) if durations else None},
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("logs", nargs="+", type=Path, help="exported vehicle runtime JSONL files")
    arguments = parser.parse_args()
    json.dump(summarize(arguments.logs), sys.stdout, ensure_ascii=False, indent=2)
    sys.stdout.write("\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
```

在 `run_offline_regressions.py` 的 `PURE_TESTS` 中加入：

```python
    ("transient hold field analysis", (sys.executable, "scripts/test/transient_hold_analyze.test.py")),
```

- [ ] **Step 4：运行，确认通过**

Run: `python3 scripts/test/transient_hold_analyze.test.py` 和 `python3 scripts/test/run_offline_regressions.py`
Expected：PASS。

- [ ] **Step 5：文档**
  - `docs/64-...`：开头状态改为"已实现（提交范围见 git log），待现场验收"。
  - `docs/07-control-and-safety.md`：新增"瞬态链路保持"一节，摘要状态模型（第 4 节表格）、故障类原因不变、制动只增不减，并链接 docs/64。
  - `docs/08-configuration.md`：在 control 配置说明中加入 `control_timeout_ms` / `bridge_apply_timeout_ms` / `transient_hold` 的字段、范围、默认值和约束（与 Global Constraints 一致）。
  - `docs/11-testing-and-validation.md`：列出新增 CTest（policy、service、integration、netem）与第 9.4 节的现场验收流程，以及分析脚本用法：`python3 scripts/test/transient_hold_analyze.py <导出的 vehicle-runtime.log>`。
  - `docs/DEPLOY.md` 第 7 节追加：瞬态保持的开启方式（使用仓库现场配置即开启；自维护配置需加 `transient_hold` 块、`control_timeout_ms: 1200`、`bridge_apply_timeout_ms: 800`），回退方式（`enabled: false`、`control_timeout_ms: 800` 后重启车端服务），以及中转 WSS 内核层调优作为现场可选项（需实测验证）。
- [ ] **Step 6：提交**

```bash
git add scripts/test/transient_hold_analyze.py scripts/test/transient_hold_analyze.test.py scripts/test/run_offline_regressions.py docs
git commit -m "Add transient hold field analysis and operator docs" -m "Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

# 第四部分　阶段 3：结构调整

## Task 23：台架与 viewer 专用头文件移出运行库

**Files:**
- Move（`git mv`）到 `cpp/bench/include/mine_teleop/bench/`：`dtls_record_trace.hpp`、`gstreamer_decoder.hpp`、`native_can_feedback.hpp`、`native_control_channel.hpp`、`native_control_courier.hpp`、`native_control_mailbox.hpp`、`native_control_publication.hpp`、`native_identity_input.hpp`、`native_mock_gear_transition.hpp`、`native_mock_input_sequence.hpp`、`native_mock_keyboard.hpp`、`native_mock_media_gate.hpp`、`native_mock_profile_retry.hpp`、`native_mock_profile_state.hpp`、`native_operator_readouts.hpp`、`native_video_health.hpp`、`native_video_layout.hpp`、`native_viewer_observation.hpp`、`native_web_control.hpp`（全部来自 `cpp/include/mine_teleop/detail/`），以及 `cpp/tests/native_receiver_core.hpp`
- Modify: 所有 include 这些文件的源文件（用下方 grep 找出）、`CMakeLists.txt`、`.github/workflows/windows-native-receiver-bench.yml`

- [ ] **Step 1：移动并改 include 路径**

```bash
mkdir -p cpp/bench/include/mine_teleop/bench
for name in dtls_record_trace gstreamer_decoder native_can_feedback native_control_channel native_control_courier \
    native_control_mailbox native_control_publication native_identity_input native_mock_gear_transition \
    native_mock_input_sequence native_mock_keyboard native_mock_media_gate native_mock_profile_retry \
    native_mock_profile_state native_operator_readouts native_video_health native_video_layout \
    native_viewer_observation native_web_control; do
  git mv "cpp/include/mine_teleop/detail/$name.hpp" "cpp/bench/include/mine_teleop/bench/$name.hpp"
  grep -rl "mine_teleop/detail/$name.hpp" cpp | xargs -r sed -i '' "s#mine_teleop/detail/$name.hpp#mine_teleop/bench/$name.hpp#g"
done
git mv cpp/tests/native_receiver_core.hpp cpp/bench/include/mine_teleop/bench/native_receiver_core.hpp
grep -rl '"native_receiver_core.hpp"' cpp | xargs -r sed -i '' 's#"native_receiver_core.hpp"#"mine_teleop/bench/native_receiver_core.hpp"#g'
```

（`sed -i ''` 为 macOS 写法。命名空间保持 `mine_teleop::detail` 不变，只移动路径。）

- [ ] **Step 2：CMake 接口库**

在 `mine_teleop_runtime` 定义之后加入：

```cmake
# Bench- and viewer-only headers; never part of the runtime library's include tree.
add_library(mine_teleop_bench INTERFACE)
target_include_directories(mine_teleop_bench INTERFACE cpp/bench/include)
target_link_libraries(mine_teleop_bench INTERFACE mine_teleop_runtime)
```

对 `grep -rl "mine_teleop/bench/" cpp` 列出的每个源文件所属的 CMake 目标，在其 `target_link_libraries` 中加入 `mine_teleop_bench`；删除 `target_include_directories(mine-teleop-windows-viewer PRIVATE cpp/tests)`。

- [ ] **Step 3：补全 Windows 工作流的路径过滤**

把 `windows-native-receiver-bench.yml` 的 `paths:` 列表替换为：

```yaml
    paths:
      - 'CMakeLists.txt'
      - 'cmake/**'
      - 'cpp/**'
      - 'configs/**'
      - 'packaging/windows/**'
      - 'scripts/build/build_windows_control_bundle.ps1'
      - '.github/workflows/windows-native-receiver-bench.yml'
```

- [ ] **Step 4：验证**

Run: `ls cpp/include/mine_teleop/detail | grep -E "^native_|dtls_record|gstreamer_decoder"` → 无输出；Task 0 Step 2；`python3 scripts/test/run_offline_regressions.py`。
Expected：全部通过。Windows viewer 的编译需在 Task 30 推送后由 CI 验证。

- [ ] **Step 5：提交**

```bash
git add -A cpp CMakeLists.txt .github/workflows/windows-native-receiver-bench.yml
git commit -m "Move bench-only headers out of the runtime include tree" -m "Co-Authored-By: Claude <noreply@anthropic.com>"
```

## Task 24：正式包关闭台架诊断（编译开关）

**Files:**
- Modify: `CMakeLists.txt`、`cpp/src/webrtc_media.cpp`、车端打包脚本中调用 CMake 的位置（用 `grep -rn "cmake -S\|cmake -B\|-DMINE_TELEOP_BUILD_VEHICLE_RUNTIME" deployments/cpp scripts/build` 找出）

- [ ] **Step 1：CMake 开关**

```cmake
option(MINE_TELEOP_ENABLE_BENCH_EXPERIMENTS "Compile isolated-mock bench diagnostics into runtime binaries" ON)
```

在 `mine_teleop_vehicle_runtime` 目标上加入：

```cmake
target_compile_definitions(mine_teleop_vehicle_runtime PRIVATE
  MINE_TELEOP_BENCH_EXPERIMENTS=$<IF:$<BOOL:${MINE_TELEOP_ENABLE_BENCH_EXPERIMENTS}>,1,0>)
```

- [ ] **Step 2：车端使用开关**

`webrtc_media.cpp` 顶部加入：

```cpp
#ifndef MINE_TELEOP_BENCH_EXPERIMENTS
#define MINE_TELEOP_BENCH_EXPERIMENTS 1
#endif
namespace { constexpr bool kBenchExperimentsCompiled = MINE_TELEOP_BENCH_EXPERIMENTS != 0; }
```

Task 9 的 `isolated_mock_trace` 初始化改为 `kBenchExperimentsCompiled && config.runtime.control_log_commands && detail::isolated_mock_bench(...)`。（offer 中的 `isolated_mock_bench_capability` 不受此开关影响，它是安全证明而不是诊断。）

- [ ] **Step 3：正式包关闭**

在 Step 0 grep 找到的每个车端打包 CMake 调用（如 `deployments/cpp/Dockerfile.build`、`deployments/cpp/Dockerfile.macos-from-scratch`）中加入 `-DMINE_TELEOP_ENABLE_BENCH_EXPERIMENTS=OFF`。

- [ ] **Step 4：验证**

Run: Task 0 Step 2（默认 ON，测试照常）；再用 `-DMINE_TELEOP_ENABLE_BENCH_EXPERIMENTS=OFF` 在另一个命名卷（如 `mine-teleop-native-build-off`）里只构建 `mine_teleop_vehicle_runtime` 和 `mine-teleop`。
Expected：两种配置都能编译；ON 时 CTest 全部通过。

- [ ] **Step 5：提交**

```bash
git add CMakeLists.txt cpp/src/webrtc_media.cpp deployments scripts/build
git commit -m "Compile bench diagnostics out of release vehicle bundles" -m "Co-Authored-By: Claude <noreply@anthropic.com>"
```

## Task 25：拆分 `run_mock_control`

**Files:**
- Create: `cpp/bench/include/mine_teleop/bench/mock_control_trace.hpp`（守卫状态、输入事件、DC 发送 trace 的有界记录与 JSON 输出）
- Create: `cpp/bench/include/mine_teleop/bench/mock_status_tracker.hpp`（`on_status` 中遥测、profile、挡位确认的状态机；时间与 UI 通过回调注入）
- Create: `cpp/tests/mock_control_trace_tests.cpp`、`cpp/tests/mock_status_tracker_tests.cpp`
- Modify: `cpp/apps/windows_native_viewer.cpp`、`CMakeLists.txt`

本任务只做"抽出可移植、可单测的部分"：线程结构（clock worker、input worker、courier 循环）保持在 viewer 中不动。新头文件不得包含 `<windows.h>`，以便在 Linux/macOS 上编译和测试。

- [ ] **Step 1：抽出 trace 记录器**
  - 新建 `MockControlTrace` 类，承接 `run_mock_control` 中的 `GuardState`、`GuardEvent`、`InputEvent` 结构，`guard_mutex`、`guard_events`、`guard_dropped`、`input_trace_mutex`、`input_events`、`input_trace_dropped`、`dc_send_trace`、`dc_send_trace_dropped`，以及 `flush_guard_trace`、`flush_dc_trace` 两个 lambda 的逻辑。
  - 接口：`void record_guard(std::int64_t at, std::int64_t authorization_at, std::int64_t authorization_now, const GuardState& state)`、`void record_input(...)`、`void record_dc_send(const nlohmann::json& record)`，以及 `std::vector<std::string> flush(std::string_view session_id)`。`flush` 返回要写到 stderr 的 JSON 行，由 viewer 负责打印，与原输出逐字段一致。
  - 每类最多保留 128 条，超出部分计数（与原行为一致）。
- [ ] **Step 2：为 trace 写测试** `mock_control_trace_tests.cpp`：覆盖 128 条上限与丢弃计数、`flush` 后清空，以及输出 JSON 的事件名（`native_mock_guard_batch`、`native_mock_input_batch`、`native_mock_dc_batch`）和字段集合与原实现一致。
- [ ] **Step 3：抽出状态跟踪器**
  - 新建 `MockStatusTracker`，承接 `on_status` lambda 中除 UI 文案外的全部逻辑：遥测新鲜度、profile 状态（`NativeMockProfileState`）、挡位确认与回退（`NativeMockGearTransition`）、安全停车计数。
  - 需要的外部动作通过构造时传入的回调完成：`report(std::wstring)` 用 `std::function<void(std::string)>` 传 UTF-8 文本，`withdraw_input`、`require_input_rearm`、`set_brake_profile`、`set_control_limits` 各一个回调。
  - viewer 中的 `on_status` 改为调用 `tracker.observe(envelope, sync_snapshot)`。
- [ ] **Step 4：为状态跟踪器写测试** `mock_status_tracker_tests.cpp`：覆盖以下四类状态，各给出输入事件序列与期望回调。
  - `control_input_rearm_required` 时要求重新按键；
  - 换挡被拒且能匹配到待定换挡时请求回退；匹配不上时冻结输入；
  - profile 失效时撤销输入授权；
  - 遥测时间戳过旧时不确认挡位。
- [ ] **Step 5：注册两个测试**（portable 测试块；链接 `mine_teleop_bench`），然后运行 Task 0 Step 2。
Expected：两个新测试在 Linux 上通过；viewer 的编译在推送后由 Windows CI 验证。
- [ ] **Step 6：提交**

```bash
git add cpp/bench cpp/apps/windows_native_viewer.cpp cpp/tests/mock_control_trace_tests.cpp cpp/tests/mock_status_tracker_tests.cpp CMakeLists.txt
git commit -m "Extract portable mock control trace and status tracking" -m "Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

# 第五部分　阶段 4：流程整理（不拆 PR）

## Task 26：测试重试标记与"正常场景硬看门狗"根因排查

**Files:**
- Modify: `cpp/tests/control_transport_integration_tests.cpp`（三处重试：`product_startup_and_p2p_cloud_recovery`、`85177ef` 引入的重按重试循环、`1f2e653` 引入的 `run < 3` 循环）

- [ ] **Step 1：加醒目标记与统计**

文件中加入全局计数与输出函数：

```cpp
std::atomic<int> g_fixture_retries{0};
void note_fixture_retry(std::string_view scenario, int attempt, std::string_view reason) {
  ++g_fixture_retries;
  std::cout << "[RETRY] scenario=" << scenario << " attempt=" << attempt << " reason=" << reason << std::endl;
}
```

三处重试在发生重试时调用 `note_fixture_retry(...)`。`main()` 在每个正常返回路径之前打印 `std::cout << "[RETRY-SUMMARY] total=" << g_fixture_retries.load() << std::endl;`。

- [ ] **Step 2：排查根因**（限时 2 小时）
  - 在 Docker 中运行 `ctest --test-dir /build -R product-direct --repeat until-fail:30 --output-on-failure`，收集出现 `[RETRY]` 的轮次的完整输出。
  - 对照车端 `control_hard_stop_transition` 诊断、驾驶端 trace 中的 sender 唤醒延迟和 socket 锁持有时间，按 superpowers:systematic-debugging 流程定位：判断硬看门狗是由夹具调度延迟引起，还是由生产代码竞态引起。
  - 结论写进 `docs/evidence/pr26-retry-root-cause-20261104.md`：现象、复现次数、证据和结论。如果是生产缺陷，另开一个任务先写失败测试再修复；如果是夹具调度问题，在结论文档中说明，并保留重试。
- [ ] **Step 3：运行** Task 0 Step 2。
- [ ] **Step 4：提交**

```bash
git add cpp/tests/control_transport_integration_tests.cpp docs/evidence/pr26-retry-root-cause-20261104.md
git commit -m "Mark integration fixture retries and record their root cause" -m "Co-Authored-By: Claude <noreply@anthropic.com>"
```

## Task 27：PR 说明重写稿

**Files:**
- Create: `docs/superpowers/plans/pr26-description-draft.md`（提交到仓库，供审阅）

- [ ] **Step 1：写重写稿**，包含以下几部分：
  - 当前状态，3–5 句；
  - 生产改动清单：ICE 回退与诊断、IDR 补发、keep-alive、`ice.*` 拒绝启动、receive-only 模式、DC 试探预算、瞬态链路保持与硬看门狗及 `bridge_apply_timeout_ms`；
  - 台架工具清单；
  - 已删除的实验：镜像、紧凑格式；
  - 升级与回退步骤；
  - 已知限制，以及验证情况：CI 链接占位由用户填写，本地测试结果如实列出，跳过的项单独列。
- [ ] **Step 2：提交**

```bash
git add docs/superpowers/plans/pr26-description-draft.md
git commit -m "Draft the rewritten PR description" -m "Co-Authored-By: Claude <noreply@anthropic.com>"
```

- [ ] **Step 3：请用户审阅重写稿。** 只有在用户明确同意后，才执行 `gh pr edit 26 --body-file docs/superpowers/plans/pr26-description-draft.md`。

## Task 28：原始证据迁出方案（需用户决定目的地）

**Files:**
- Create: `docs/evidence/README.md`（证据索引：每个文件一行，说明内容、日期、是否已失效）

- [ ] **Step 1：写索引**，并列出建议迁出的原始 JSON 文件及其大小（`ls -la docs/evidence/*.json`）。
- [ ] **Step 2：提交索引**

```bash
git add docs/evidence/README.md
git commit -m "Index PR #26 evidence files" -m "Co-Authored-By: Claude <noreply@anthropic.com>"
```

- [ ] **Step 3：向用户确认迁出目的地**（GitHub Release 附件、内部存储或保留在仓库）。这属于对外发布或删除历史数据，未获明确同意前不执行任何迁移或删除。

## Task 29：Windows CI 验证（需用户同意推送）

- [ ] **Step 1：向用户说明需要推送的原因**：Task 6–9、23、25 修改了 Windows-only 代码，只能由 Windows 工作流编译验证。征得同意后执行 `git push origin rjr/cx/native-realtime-pr26`。
- [ ] **Step 2：观察 CI**：`gh pr checks 26 --watch`。全部检查（含 `Windows x64 receive-only probe`）通过才算完成；如有失败，修复后重新推送（每次推送前都需同意，或者用户已明确授权本轮多次推送）。

## Task 30：最终验证与汇报

- [ ] **Step 1：全量验证**：Task 0 Step 2 的两条命令；`grep -rnE "mock_dual|MockDual|mock_wss_dc|MockWssDc|mock_compact|MockCompact|NativeMirrorIngressAcks|mock_media_status_v1|mock_dc_backpressure|mock_skip_dc|send_mock_compact|wss_dc_mirror" cpp CMakeLists.txt` 无输出；`git status` 干净。
- [ ] **Step 2：按 superpowers:verification-before-completion 汇报**：每一项都附命令输出；列出跳过和未执行的项（例如 netem 被跳过、Windows CI 未推送、现场验收未做），不得把它们算作通过。
- [ ] **Step 3：询问合并方式**（审查 D3）：向用户确认 PR 合并时采用 squash 还是保留提交历史；本计划不改写任何已推送的提交。


---

## 执行偏差记录（阶段 2，Task 6–11）

- **Task 6：** 按计划，外加 Step 0 把 `isolated_mock_control_allowed` 暂移到 `control_transport.hpp`。额外发现并修改 `control_tests.cpp`
  里"云端原样转发 offer 扩展字段"的用例：先保留 `mock_wss_dc_mirror`，Task 7 换成中性的 `extension_probe`。通道测试在 GStreamer 1.20
  （本地与 CI 实际走的分支）上只剩"拒绝建立控制通道"的 fail-closed 检查。
- **Task 7：** 车端和 viewer 的"DC 媒体状态镜像"（原属 Task 8）提前并入本任务，因为它依赖被删除的 `mock_dual_control_enabled()`，
  否则该提交无法编译。集成测试夹具构造函数变为 `Fixture(direct, feedback, product, mode, defer_owner)`；`feedback_vehicle_method_tests.cpp`
  的夹具要同步删除假的 `g_object_get`、`buffered_bytes` 和相关局部变量（`.inc` 由 `extract_feedback_vehicle_method.py` 从 `webrtc_media.cpp`
  抽取，源码改动会直接改变被测代码）。顺带删除了 `<cstdlib>`（仅为 `getenv` 引入）。`control_transport_tests` 的用例数实际是 16。
  发布过滤的 WSS 路径保护与"页面不能给已签名字节加字段"两条断言以中性形式保留。
- **Task 8：** 见上，改为恢复而不是打补丁。
- **Task 9：** 车端以 `isolated_mock_bench_attested()` / `isolated_mock_trace()` 两个成员实现；Python 假 `Impl` 用一个可开关的
  `attested` 成员，另加源码契约检查：实现必须使用共享谓词，且 `webrtc_media.cpp` 里不得再出现 `starts_with("test_bench_")`。
- **Task 10：** 守卫放在 `cpp/tests/owned_callback_guard.hpp`（与 `native_receiver_core.hpp` 同目录），Task 23 一起移动。macOS 分支用本机 vcpkg 的
  GStreamer/libnice 头文件做了语法检查（`/tmp/mt-tools/mac-syntax.sh`），Linux 构建覆盖非 macOS 分支。
- **Task 11：** 只有 `windows-native-viewer.md` 和 4 个证据文件需要处理；`native-receiver-bench.md` 等里的"镜像/dual"是 Homebrew 镜像、
  双栈 IP 等无关含义，未改。
- **未验证项：** `cpp/apps/windows_native_viewer.cpp` 在阶段 2 被修改（Task 6、7、9），本机没有 Windows 工具链，只做了定界符平衡检查；
  必须推送后由 Windows 工作流编译验证（Task 29，需用户同意推送）。

## 执行偏差记录（阶段 3，Task 12–22）

下列偏差都在执行中发现，原因和验证方式一并记录；没有一项降低了计划要求的覆盖。

**Task 12**
- 策略头文件与测试逐字取自计划（草稿目录里先做过变异检查）。随后在 Task 16 增加了两个纯函数和一个穷举等价性测试（见 Task 16）。

**Task 13**
- 在 `control.transient_hold` 块里额外拒绝未知键（拼错的键不会静默回退到默认值，`hold_brake` 是安全相关参数）、拒绝非映射写法；空块（`transient_hold:`）视为未配置。对应测试在计划用例之外补充了边界值（`degraded+max_hold+100` 恰好等于/少 1 ms）、逐字段越界、现场配置自身的断言、`redacted_summary`、其他配置保持关闭。
- `speed_feedback_timeout_ms` 校验改为与 `bridge_apply_timeout_ms` 比较，报错文案补充了该字段名（原文案是合并的，没有点名 `control_timeout_ms`）。

**Task 14**
- 已认证的 ESTOP 不依赖可选的保持字段：`validate()` 对 ESTOP 跳过"瞬态视频标志"一致性规则；`from_json` 对 ESTOP 忽略未知或矛盾的保持字段（置为未协商），非 ESTOP 仍然拒绝。计划的写法会让一个带异常保持字段的 ESTOP 被拒收，与既有注释"ESTOP 不依赖更新的生产者 schema"矛盾。
- 云端信令对 `control_command` 用 `ControlCommand::from_json` 校验并原样转发 payload，所以新字段在中转链路上保持不变；MAC 覆盖整个 payload（除 `native_mac` 本身），无需另行签名。

**Task 15**
- 制动下限不用存储字段 `brake_floor_`，改为 `current_brake_floor()` 由当前状态即时推导（制动等待或恢复制动保持 → 1.0；保持中 → `hold_brake`）。两者生命周期等价，但少一个会失步的可变成员。
- 新增 `end_transient_hold(reason)`：硬超时、安全锁存、ESTOP、其他故障启动制动等待、驾驶端撤回能力时，保持记录明确结束并写入原因。否则硬停车之后页面仍会显示"保持中"的横幅。
- 间隔/迟到的保持路径只在没有制动等待时启用；已在制动等待时沿用原有写法（副作用保持不变）。
- 计划的夹具里 `control_link_ready=false` 没有同时撤回 `operator_input_ready`，会被 `ControlCommand::validate` 当作无效命令拒收，到不了制动等待；夹具已修正。
- 服务测试 19 个用例（计划 13 个 + 刹车保留、硬超时看门狗停滞、能力撤回、禁用的车端、事件、ESTOP 后记录等），15 个变异体全部被杀死。

**Task 15（补记，由 Task 20 的集成测试发现）**
- 看门狗里"`DEGRADED` 就进入控制链路中断保持"是错的（计划 Step 6 也是这样写的）：`set_control_braking_wait(false)` 经 `require_input_rearm()` 会强制把状态置为 `DEGRADED`，此时命令一直在到达，并没有断流。结果是每次制动等待恢复后都会产生一次假的 `control_gap` 保持：占用保持预算、横幅和日志误报"链路中断"。改为必须同时满足"距最后一条有效命令 ≥ `degraded_timeout_ms`"（新增成员 `degraded_timeout_ms_`）。服务层补回归用例 `braking_wait_release_does_not_fake_a_gap`，红灯→绿灯，并新增 2 个变异体（共 17 个，全部被杀死）。

**Task 16**
- 计划里按"任何非零异常即过期"调用分类函数，在未协商时会让 1–2 帧丢帧也触发制动（旧规则是连续 3 帧才算）。改为两个纯函数 `effective_lane_anomalies`、`vehicle_video_lane_blocks_control`，并用穷举测试证明未协商时与旧布尔表达式逐位一致（7 位输入 × 异常数 0–4）。
- 保持事件在持锁块之后、启动宽限期的提前 `return` 之前发出；计划放在函数末尾，宽限期内的事件会被取走却不发出。该缺陷由 `recovery_enforcer_tests` 的新用例抓到。
- 硬停车诊断（`control_hard_stop_transition`）已整体嵌入 `control_timing`，Task 15 之后其中自带 `transient_hold`，不再另加重复键。
- `recovery_enforcer_tests` 的伪造夹具补了新成员，并新增 7 个行为用例，直接执行从 `webrtc_media.cpp` 抽取的真实代码，配真实闩锁和真实服务；9 个变异体全部被杀死。
- `ice_vehicle_callbacks.test.py` 用行为用例（真实抽取的 offer 构造代码 + 驾驶端的解析函数）替代计划里的字符串存在性检查，另保留代际变化的契约检查。

**Task 17**
- 计划把"协商 + 负载"都放在一个单元用例里验证；协商（含越界容忍值、缺失能力清除旧协商、Windows mock 客户端关闭）在单元层覆盖，命令负载里的 `transient_hold` / `critical_video_transient` 赋值由 Task 20 的真实回环集成测试端到端覆盖（车端真的收到带保持字段的命令并进入保持），并在那里对赋值做变异验证。
- `/api/control-intent` 的意图回显和发送路径追踪里补了 `critical_video_transient` / `transient_hold_capable`，便于现场日志核对。
- Windows viewer 只加一行 `config.disable_transient_hold = true;`，无法在本地编译，待获准推送后由 Windows 工作流验证。

**Task 18**
- 容忍值在持有 WSS 锁的块开头设置，早于本周期首个 ACK 的消费（排空、DC 的 `observe`、WSS 的 `observe`），而不是计划写的"`observe` 之前"：计划的位置会让容忍值滞后一个采样周期。
- 草稿目录里的 ACK 头文件基于旧版本（含已删除的镜像类），只套用与容忍相关的改动，不整体拷贝。
- 在计划的 4 个用例之外补了 2 个：重连套接字会重置异常计时，以及负容忍值等同 0。11 个变异体全部被杀死；其中"负容忍值"一开始存活（行为等价，只有访问器返回负数），补断言后被杀死。

**Task 19**
- 新增纯函数 `transientHoldPresentation(runtime, telemetry)` 生成监控面板"链路保持"一行，页面只调用它，因此可以单测（计划里是直接写在内嵌页面里）。
- `setTransientHoldNegotiated` 在 `renderMonitoring()` 里调用，而不是只在 `/api/status` 轮询赋值处，因为后者在登录、连接、恢复等多处赋值，漏一处就会在协商生效后仍按旧规则推进制动修订号。位置是在 ESTOP 呈现和恢复呈现之后、`renderVcuHandshake()` 之前：首版放在函数开头，被 `control_tests` 的 `control_page_contract` 抓到——该契约要求 `function renderMonitoring(){const estopPresentation=…`，即急停横幅必须是监控刷新做的第一件事，这条约束应当保留，所以改动放到后面而不是放宽断言。这是完整套件才暴露的回归（任务 19 的目标测试都通过）。
- 页面测试追加在各自文件的汇总语句之前（`camera_view.test.cjs` 的 `process.exitCode`、`control_logic.test.cjs` 的 `Promise.all(pendingTests)` 汇总块）。
- 既有的 8 个"actual shipped monitor …"用例在沙箱里执行真实的 `renderMonitoring()`，新增的 `cameraView` 调用让它们抛 `ReferenceError`。修正放在测试夹具 `useProductionCameraView`：把它已创建的真实 camera view 同时挂到 `context.cameraView`，监控用例因此会执行真实的 `setTransientHoldNegotiated`，而不是放一个桩。
- 页面变异检查共 27 个变异体：首轮 3 个存活（"已结束且静音的轨道也不是瞬态"、"保持中又进入制动等待时沿用重按提示"、"页面只在保持状态变化时记录日志且保持期间不清输入"），补测试后全部被杀死。后者通过从页面源码抽出真实的 `applyVehicleSafetyState` 在沙箱里执行来验证。
- 未改动 `docs/45-control-stop-trigger-review.md` 里"画面中断/暂无新帧提示保留"的措辞（它记录的是当时的评审结论）；页面文案现在是"画面短暂中断"，Task 22 的文档里统一说明。

**Task 20**
- 计划里 `car_watchdog` 线程只在 `direct_enabled` 时启动，而场景用非直连夹具；不启动车端定时 tick，断流期间车端不会进入保持。夹具改为 `direct_enabled || transient_hold || tick_vehicle` 时启动。
- 夹具原先的会话配置目标车速为 0，油门恒为 0，无法断言"牵引被撤回/恢复"；瞬态夹具改用非零配置并保留 `MockVehicleAdapter` 指针，在同一把锁内同时读取时序 JSON 和适配器输出，避免读到不一致的快照。
- 夹具没有 `VehicleMediaRuntime` 的恢复协调器，所以和既有安全夹具一样，在传输选择引起的初始制动等待上由测试显式释放；释放后先等车端接受中性命令（`rearm_required == false`）再按下油门，与页面的流程一致。
- 计划里的状态键 `intent_requires_fresh_input` 在驾驶端 `status()` 里实际叫 `native_control.requires_fresh_input`，用 `.value(..., false)` 会空过；改用 `.at()`。`transient_hold.last` 在从未保持过时是 `null`，对它调用 `.value()` 会抛 JSON 类型错误而不是给出断言信息；加了空安全的读取函数。
- **挂钟会倒退。** 在这台 Colima VM 里实测 60 秒内 `CLOCK_REALTIME` 倒退 5 次，最大约 104 ms（约每 12 秒一次，像是时间同步服务在校正）。夹具把 `utc()`（挂钟）同时当作车端服务的单调时钟喂入，保持按设计在看到时间倒退时 fail-closed（`clock_reversed`），于是约 10% 的运行出现 0–47 ms 的假升级。生产里车端用 `steady_clock`，不受影响。修复：`LogicalTimeControlService` 增加可注入单调时钟的构造重载（不改变其他测试），只有新场景使用 `steady()`。修复后 42 次连续运行零失败。
- 新增对照场景：同一次断流下没有保持能力的车端仍进入制动等待，并断言驾驶端不会向没有声明能力的车端发送 `transient_hold` 字段；另断言保持场景里驾驶端的签名命令确实带了该字段。
- 集成层变异：8 个里 7 个被杀死，包括任务 17 欠的两个驾驶端变异（从不声明能力 / 从不解析 offer）和"无条件声明能力"。存活的"迟到命令不触发保持"在回环场景里等价（断流结束后积压旧命令涌入时保持已经由断流进入），已由服务层 `late_commands_hold_then_resume` 和 M06 变异覆盖。
- 记入 Task 26：其他集成夹具仍把 `utc()` 当作车端单调时钟，挂钟每约 12 秒倒退约 100 ms，会让它们偶发出现 `receive_time_reversed`、硬看门狗等现象。这很可能就是 Task 26 要查的"正常场景触发硬看门狗"的根因之一。

**Task 21**
- 沿用 Task 20 的辅助函数；`#include <cstdlib>`/`<unistd.h>` 放在文件顶部。这台 Colima VM 支持 `sch_netem`，所以该测试真正执行并通过（4.85 秒，不是跳过）；不支持的环境返回 77，CTest 记为跳过。
- 设计 9.3 写的是 netem 延迟尖峰，实现用 `loss 100%`（350 ms 与 1500 ms）：丢包才会触发 TCP 重传退避和队头阻塞，350 ms 的丢包使车端投递间隔延长到约 600 ms，正好检验保持能吸收它，同时驾驶端 ACK 容忍（Task 18）不让驾驶端先撤回。
- 首版断言只有"没有升级"，变异体"丢包根本没生效"存活，说明测试可能空过；补了"丢包确实打断了车端投递（`saw_hold`）"和"保持期间牵引为 0 且有轻制动"。最终 4 个变异体（驾驶端容忍关闭、ACK 停滞规则退回旧值、车端从不保持、丢包未生效）全部被杀死，连续 12 次运行无失败也无跳过。

**Task 22**
- 分析脚本与计划原文逐字一致；测试在计划的 1 个用例外补了"多文件合并、百分位、未知升级原因、空日志统计为 null"3 个用例，8 个变异体全部被杀死。已用真实策略产出的事件载荷验证脚本能正确统计。
- 文档除计划列出的 `64/07/08/11/DEPLOY.md` 外，还给 `docs/24-vehicle-runtime-diagnostics.md` 加了三个事件的条目（运维按它查事件），并在 `docs/64` 增加第 14 节"实施记录"和第 15 节"现场验收清单"（含 `escalation_reason` 的全部取值，其中 `hard_timeout`/`safety_latched`/`estop` 表示更强的停车接管了保持）。
- `DEPLOY.md` 里 `rto_min` 路由属性已在容器的 iproute2 中确认存在，并明确写成"现场实测后再采用，代码默认不变"。
- 没有改动 `docs/45-control-stop-trigger-review.md`（它记录当时的评审结论，其中"画面中断/暂无新帧提示保留"是历史描述）。

**已知环境偶发（与阶段 3 无关）**
- `mine-teleop-media-time-sync-retry-loopback`（`cli-over-budget`）：在阶段 2 结束的基线二进制上 4/12 失败，阶段 3 二进制 4/12 失败；空闲与 8 个满载进程下失败率相同（2/8）。完整 CTest 约三分之一的运行会因它失败，它是唯一失败项时视为通过并如实说明。留给 Task 26 查根因。
- `mine-teleop-control-tests` 的 `driver_login_failure_rate_limit_and_recovery`：完整运行中失败过 1 次，5 次隔离复跑通过；同样留给 Task 26。

**阶段 3 的验证概况与未验证项**
- 提交：Task 12–22 共 12 个提交（Task 15 的缺陷修复单独一个）。每个提交都在 Ubuntu 22.04 容器里对最终树跑了完整 CTest，并跑了与改动对应的变异检查（策略 11 个以上、服务 17 个、媒体观察者 9 个、ACK 11 个、页面 27 个、集成 8 个、netem 4 个、分析脚本 8 个，最终全部被杀死，个别等价变异已说明）。
- 完整 CTest 现为 93 项，全部通过；其中 `mine-teleop-transient-hold-netem-tests` 真正执行（不是跳过）。
- **未验证：** `cpp/apps/windows_native_viewer.cpp`（阶段 2 的改动加上阶段 3 的一行 `config.disable_transient_hold = true;`）仍不能在本机编译，需推送后由 Windows 工作流验证；真实 WebRTC DataChannel、真实 5G/TURN 链路和实车都没有覆盖，只有回环 WSS 和回环丢包；现场验收清单见 `docs/64` 第 15 节。
