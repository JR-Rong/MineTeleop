const controlLogic=MineTeleopControlLogic;
async function bootstrapConsole(){
let consoleConfig;
try{
  const consoleConfigResponse=await fetch('/api/console-config',{cache:'no-store'});
  if(!consoleConfigResponse.ok)throw new Error(`console config unavailable: ${consoleConfigResponse.status}`);
  consoleConfig=await consoleConfigResponse.json();
  if(!consoleConfig||typeof consoleConfig!=='object'||!consoleConfig.page_capability)throw new Error('console config missing page capability');
}catch(error){
  const bootstrapStatus=document.getElementById('status');
  if(bootstrapStatus)bootstrapStatus.textContent='控制台配置加载失败；驾驶控制未启用';
  throw error;
}
const pageCapability=consoleConfig.page_capability;
const gamepadConfig=consoleConfig.gamepad||{};
const limitConfig=consoleConfig.control_limits||{};
const keys=controlLogic.KEY_BINDINGS;
let state=controlLogic.deriveKeyState(controlLogic.createKeySet());
const pressedControlKeys=controlLogic.createKeySet(),blockedControlKeys=controlLogic.createKeySet();
const gamepadState={connected:false,steering:0,throttle:0,brake:0};
const driverActuationDefaults={target_speed_kph:limitConfig.initial_target_speed_kph,max_motor_torque_nm:limitConfig.initial_max_motor_torque_nm,max_brake_pressure_bar:limitConfig.initial_max_brake_pressure_bar,service_brake_pressure_bar:limitConfig.initial_service_brake_pressure_bar,hard_brake_pressure_bar:limitConfig.initial_hard_brake_pressure_bar,max_steering_angle_deg:limitConfig.initial_max_steering_angle_deg};
let controlProfileState={requestedProfile:null,pendingRequestSeq:0,effectiveProfile:null,effectiveRequestSeq:0,effectiveAppliedRevision:0,acknowledged:false,reason:''},pendingControlProfileEnvelope=null,lastControlProfileSendAt=0,controlProfilePrepareInFlight=false,controlProfileGeneration=0;
let vehicleHardLimits={received:false};
const controlTraceEnabled=Boolean(consoleConfig.control_trace_commands),intentRefreshIntervalMs=Math.max(50,Math.floor(Number(consoleConfig.intent_lease_ms||200)/3)),controlTraceBuffer=[],controlTraceErrorScopes=new WeakMap();
let controlTraceSummary=createControlTraceSummary(),controlTraceScope={session_id:'',vehicle_id:''},lastHeartbeatTraceAt=null;
const calibration={steeringCenter:gamepadConfig.steering_center,steeringRange:gamepadConfig.steering_range,throttleRest:gamepadConfig.throttle_rest,throttleRange:gamepadConfig.throttle_range,brakeRest:gamepadConfig.brake_rest,brakeRange:gamepadConfig.brake_range};
const webrtcLabel=document.getElementById('webrtc'),cameraGrid=document.getElementById('cameras'),statusPanel=document.getElementById('status'),loginPanel=document.getElementById('login-panel'),sessionPanel=document.getElementById('session-panel'),passwordInput=document.getElementById('password'),vehicleSelect=document.getElementById('vehicle'),connectButton=document.getElementById('connect'),authExpiry=document.getElementById('auth-expiry'),vcuPanel=document.getElementById('vcu-panel'),vcuStatus=document.getElementById('vcu-status'),vcuGate=document.getElementById('vcu-gate'),vcuConnectButton=document.getElementById('vcu-connect'),vcuDisconnectButton=document.getElementById('vcu-disconnect'),controlLimitsOpen=document.getElementById('control-limits-open'),controlLimitsSummary=document.getElementById('control-limits-summary'),controlLimitsDialog=document.getElementById('control-limits-dialog'),targetSpeedKph=document.getElementById('target-speed-kph'),maxMotorTorqueNm=document.getElementById('max-motor-torque-nm'),maxBrakePressureBar=document.getElementById('max-brake-pressure-bar'),serviceBrakePressureBar=document.getElementById('service-brake-pressure-bar'),hardBrakePressureBar=document.getElementById('hard-brake-pressure-bar'),maxSteeringDeg=document.getElementById('max-steering-deg'),speedPidKp=document.getElementById('speed-pid-kp'),speedPidKi=document.getElementById('speed-pid-ki'),speedPidKd=document.getElementById('speed-pid-kd'),speedPidDerivativeFilterTauMs=document.getElementById('speed-pid-derivative-filter-tau-ms'),speedPidMaxDtMs=document.getElementById('speed-pid-max-dt-ms'),motorTorqueRiseRate=document.getElementById('motor-torque-rise-rate'),vehicleHardLimitsLabel=document.getElementById('vehicle-hard-limits'),controlLimitsConfirm=document.getElementById('control-limits-confirm'),controlLimitsApply=document.getElementById('control-limits-apply'),controlLimitsCancel=document.getElementById('control-limits-cancel'),estopStatus=document.getElementById('estop-status'),monitorPanel=document.getElementById('monitor-panel'),alertsPanel=document.getElementById('alerts'),streamMetrics=document.getElementById('stream-metrics'),inputReadiness=document.getElementById('input-readiness'),lastKeyboardEvent=document.getElementById('last-keyboard-event'),emptyStage=document.getElementById('empty-stage');
const canFeedbackPanel=document.getElementById('can-feedback-panel'),canFeedbackStatus=document.getElementById('can-feedback-status'),canSpeed=document.getElementById('can-speed'),canGear=document.getElementById('can-gear'),canSelector=document.getElementById('can-selector'),canEpb=document.getElementById('can-epb'),canHandshake=document.getElementById('can-handshake'),canVmcFault=document.getElementById('can-vmc-fault'),canParkingSwitch=document.getElementById('can-parking-switch'),canBrakePedal=document.getElementById('can-brake-pedal'),canEmergency=document.getElementById('can-emergency'),canAge=document.getElementById('can-age'),wheelFeedbackGrid=document.getElementById('wheel-feedback-grid'),steeringFeedbackGrid=document.getElementById('steering-feedback-grid');
const operatorSpeed=document.getElementById('operator-speed'),operatorActualGear=document.getElementById('operator-actual-gear');
const keyIndicators={left:document.getElementById('key-left'),right:document.getElementById('key-right'),up:document.getElementById('key-up'),down:document.getElementById('key-down'),service_brake:document.getElementById('key-service-brake'),hard_brake:document.getElementById('key-hard-brake')};
const controlReadouts={gear:document.getElementById('control-gear'),steering:document.getElementById('control-steering'),throttle:document.getElementById('control-throttle'),brake:document.getElementById('control-brake')};
const operatorControlReadouts={gear:document.getElementById('operator-control-gear'),steering:document.getElementById('operator-control-steering'),throttle:document.getElementById('operator-control-throttle'),brake:document.getElementById('operator-control-brake')};
let peer=null,controlChannel=null,pendingIce=[],remoteCameraIds=[],offeredCameraByMid=new Map(),iceServers=[],polling=false,connecting=false,authenticated=false,mediaStatus={lanes:[]},h265FailureSamples=0,h265FallbackSent=false,estopLatched=false,gamepadEstopPressedAt=0,gamepadRequiresNeutral=true,activeGamepadIndex=null,latestMetrics={streams:[]},latestRuntimeStatus={},lastAlertKey='',controlAuthorityLost=false,gearRejectionInhibited=false,signalingGeneration=0,signalingPollAbort=null,vehicleTelemetry=null,lastVehicleSafetyState='',vcuHandshake={supported:false,state:'unavailable',ready:false,requested:false,disarming:false,parking_ready:false,driver_connected:false,adapter_ready:null},vcuEverReady=false,selectedGear='N',pendingGearRequest=null,pendingGearTransition=null,gearTransitionGeneration=0,lastControlStatusSeq=0,activeControlPrepareAbort=null,activeControlPrepareIsEstop=false,activeControlPreparePreemptedByEstop=false;const previousStats=new Map(),cameraByMid=new Map(),assignedCameraIds=new Set();
let gearChangeStationaryEvidence=controlLogic.createGearChangeStationaryEvidence();
const uiInstanceId=(globalThis.crypto?.randomUUID?.()||`ui-${Date.now()}-${Math.random().toString(16).slice(2)}`).replace(/[^A-Za-z0-9_-]/g,'_');
let nativeIntentSeq=0,lastNativeIntentSnapshot='',nativeControlSessionId='',nativeControlSessionGeneration=0;
let controlOutcomeSession={metrics:controlLogic.createControlOutcomeMetrics()};
function responseError(response,body){const error=Error(body.error||response.status);error.status=response.status;return error}
async function post(path,body={},signal=null){const options={method:'POST',headers:{'content-type':'application/json','x-mine-teleop-page-capability':pageCapability},body:JSON.stringify(body)};if(signal)options.signal=signal;const r=await fetch(path,options);const j=await r.json();if(!r.ok)throw responseError(r,j);return j}
async function get(path){const r=await fetch(path);const j=await r.json();if(!r.ok)throw responseError(r,j);return j}
function clientLog(event,details={}){const entry={event,sent_at_utc_ms:Date.now(),details};console.info(JSON.stringify(entry));fetch('/api/browser-event',{method:'POST',headers:{'content-type':'application/json','x-mine-teleop-page-capability':pageCapability},body:JSON.stringify(entry),keepalive:true}).catch(()=>{})}
function createControlTraceSummary(){return{interval_started_at_utc_ms:Date.now(),heartbeat_tick_count:0,heartbeat_enqueued_count:0,heartbeat_coalesced_count:0,timer_lag_sample_count:0,timer_lag_total_ms:0,timer_lag_max_ms:0,explicit_send_count:0,backpressure_count:0,max_buffered_amount_bytes:0,prepare_timeout_count:0,prepare_expired_count:0,prepare_preempted_by_estop_count:0,queue_unhandled_error_count:0}}
function noteIntentRefresh(now){if(!controlTraceEnabled)return;controlTraceSummary.heartbeat_tick_count++;if(lastHeartbeatTraceAt!==null){const lag=Math.max(0,now-lastHeartbeatTraceAt-intentRefreshIntervalMs);controlTraceSummary.timer_lag_sample_count++;controlTraceSummary.timer_lag_total_ms+=lag;controlTraceSummary.timer_lag_max_ms=Math.max(controlTraceSummary.timer_lag_max_ms,lag)}lastHeartbeatTraceAt=now}
function setControlTraceScope(sessionId='',vehicleId=''){if(!controlTraceEnabled)return;const next={session_id:String(sessionId||''),vehicle_id:String(vehicleId||'')};if(next.session_id===controlTraceScope.session_id&&next.vehicle_id===controlTraceScope.vehicle_id)return;if(controlTraceScope.session_id||controlTraceScope.vehicle_id)flushControlTrace('session_scope_change');controlTraceScope=next}
function sameControlTraceScope(scope){return Boolean(scope)&&String(scope.session_id||'')===controlTraceScope.session_id&&String(scope.vehicle_id||'')===controlTraceScope.vehicle_id}
function emitControlTraceBatch(reason,scope,commands,summary,sampleTransport=true){const samples=summary.timer_lag_sample_count;summary.interval_ended_at_utc_ms=Date.now();summary.timer_lag_average_ms=samples?summary.timer_lag_total_ms/samples:0;summary.peer_connection_state=sampleTransport?(peer?.connectionState||'none'):null;summary.data_channel_ready_state=sampleTransport?(controlChannel?.readyState||'none'):null;summary.data_channel_buffered_amount_bytes=sampleTransport?Math.max(0,Number(controlChannel?.bufferedAmount)||0):null;clientLog('control_trace_batch',{reason,trace_session_id:String(scope&&scope.session_id||''),trace_vehicle_id:String(scope&&scope.vehicle_id||''),commands,summary})}
function noteScopedControlTraceCounter(field,scope){if(!controlTraceEnabled)return;if(sameControlTraceScope(scope)){controlTraceSummary[field]++;return}const summary=createControlTraceSummary();summary[field]=1;emitControlTraceBatch(`late_${field}`,scope,[],summary,false)}
function flushControlTrace(reason='interval'){if(!controlTraceEnabled)return false;const summary=controlTraceSummary,scope=controlTraceScope,commands=controlTraceBuffer.splice(0,controlTraceBuffer.length),hasActivity=commands.length||summary.heartbeat_tick_count||summary.explicit_send_count||summary.backpressure_count||summary.prepare_timeout_count||summary.prepare_expired_count||summary.prepare_preempted_by_estop_count||summary.queue_unhandled_error_count;controlTraceSummary=createControlTraceSummary();if(!hasActivity)return false;emitControlTraceBatch(reason,scope,commands,summary);return true}
function resetControlOutcomeSession(){controlOutcomeSession={metrics:controlLogic.createControlOutcomeMetrics()}}
function hasTurnServer(){return iceServers.some(server=>String(server.urls||'').includes('turn:')||(Array.isArray(server.urls)&&server.urls.some(url=>String(url).startsWith('turn:'))))}
function safeIceEndpoint(value){const match=String(value||'').match(/^([a-z]+):(?:\/\/)?(?:[^@]*@)?(\[[^\]]+\]|[^:?/]+)(?::(\d+))?/i);return match?`${match[1].toLowerCase()}:${match[2]}${match[3]?`:${match[3]}`:''}`:'unknown'}
function setMetric(id,text,level=''){const element=document.getElementById(id);element.textContent=text;element.classList.remove('ok','warn','critical');if(level)element.classList.add(level)}
function formatMetric(value,digits=1,suffix=''){return Number.isFinite(Number(value))?`${Number(value).toFixed(digits)}${suffix}`:'未知'}
function setCanValue(id,text){const element=document.getElementById(id);if(element)element.textContent=text}
function validCanValue(values,valid,index,digits,unit=''){return Array.isArray(values)&&Array.isArray(valid)&&valid[index]&&Number.isFinite(Number(values[index]))?`${Number(values[index]).toFixed(digits)}${unit}`:'—'}
function ensureCanFeedbackCards(){
  if(!wheelFeedbackGrid.childElementCount)for(let index=0;index<8;index++){const card=document.createElement('article');card.className='wheel-feedback';const title=document.createElement('div');title.className='feedback-title';const name=document.createElement('strong');name.textContent=`轮端 ${index+1}`;const mode=document.createElement('span');mode.id=`can-wheel-mode-${index}`;mode.textContent='M— / B—';title.append(name,mode);const values=document.createElement('div');values.className='feedback-values';for(const [label,key] of [['扭矩','torque'],['转速','speed'],['制动压力','brake']]){const item=document.createElement('div');item.className='feedback-value';const caption=document.createElement('span');caption.textContent=label;const value=document.createElement('strong');value.id=`can-wheel-${key}-${index}`;value.textContent='—';item.append(caption,value);values.appendChild(item)}card.append(title,values);wheelFeedbackGrid.appendChild(card)}
  if(!steeringFeedbackGrid.childElementCount)for(let index=0;index<4;index++){const card=document.createElement('article');card.className='steering-feedback';const title=document.createElement('div');title.className='feedback-title';const name=document.createElement('strong');name.textContent=`转向轴 ${index+1}`;title.appendChild(name);const values=document.createElement('div');values.className='feedback-values';for(const [label,key] of [['转角','angle'],['模式','mode']]){const item=document.createElement('div');item.className='feedback-value';const caption=document.createElement('span');caption.textContent=label;const value=document.createElement('strong');value.id=`can-steering-${key}-${index}`;value.textContent='—';item.append(caption,value);values.appendChild(item)}card.append(title,values);steeringFeedbackGrid.appendChild(card)}
}
function renderCanFeedback(){
  canFeedbackPanel.hidden=!authenticated;
  if(!authenticated)return;
  ensureCanFeedbackCards();
  const feedback=vehicleTelemetry?.can_feedback;
  const supported=Boolean(feedback?.supported);
  const transportAge=vehicleTelemetry&&Number.isFinite(Number(vehicleTelemetry.sent_at_utc_ms))?Math.max(0,Date.now()-Number(vehicleTelemetry.sent_at_utc_ms)):null,canFeedbackAge=supported&&Number(feedback.max_feedback_age_ms)>=0?Number(feedback.max_feedback_age_ms):null;
  canFeedbackStatus.textContent=!vehicleTelemetry?'等待车端遥测':(supported?`序号 ${vehicleTelemetry.seq??'—'}`:'车端桥接包无完整反馈');
  canFeedbackStatus.className=supported&&Boolean(feedback.feedback_fresh)&&transportAge!==null&&transportAge<=500?'status-chip ok':'status-chip warn';
  const telemetrySpeed=vehicleTelemetry?.speed_mps,measuredSpeed=supported&&feedback.speed_valid?Number(feedback.speed_mps):(telemetrySpeed!==null&&Number.isFinite(Number(telemetrySpeed))?Number(telemetrySpeed):null),speedText=measuredSpeed===null?'—':`${measuredSpeed.toFixed(2)} m/s · ${(measuredSpeed*3.6).toFixed(1)} km/h`;
  const gearText=supported&&feedback.gear_valid?vcuGearLabel(feedback.gear):(typeof vehicleTelemetry?.gear==='string'&&vehicleTelemetry.gear?vehicleTelemetry.gear:'—');
  canSpeed.textContent=speedText;operatorSpeed.textContent=speedText;
  canGear.textContent=gearText;operatorActualGear.textContent=gearText;
  canSelector.textContent=supported&&feedback.driver_gear_request_valid?vcuGearLabel(feedback.driver_gear_request):'—';
  canEpb.textContent=supported&&Array.isArray(feedback.parking_brake_status)?feedback.parking_brake_status.map((value,index)=>feedback.parking_brake_valid?.[index]?vcuEpbLabel(value):'—').join('/'):'—';
  canHandshake.textContent=supported&&feedback.handshake_valid?String(feedback.handshake_status):'—';
  canVmcFault.textContent=supported&&feedback.vmc_fault_code_valid?String(feedback.vmc_fault_code):'—';
  canVmcFault.className=supported&&feedback.vmc_fault_code_valid&&Number(feedback.vmc_fault_code)!==0?'critical':'';
  canParkingSwitch.textContent=supported&&feedback.parking_brake_switch_valid?vcuSwitchLabel(feedback.parking_brake_switch,'已拉起','已松开'):'—';
  canBrakePedal.textContent=supported&&feedback.brake_pedal_switch_valid?vcuSwitchLabel(feedback.brake_pedal_switch,'已踩下','已松开'):'—';
  canEmergency.textContent=supported&&feedback.gear_valid?String(feedback.emergency_switch??'—'):'—';
  canAge.textContent=!supported?'—':(canFeedbackAge===null?'CAN 未完整':`${canFeedbackAge} ms CAN · ${transportAge??'—'} ms 链路`);
  canAge.className=supported&&Boolean(feedback.feedback_fresh)&&transportAge!==null&&transportAge<=500?'ok':'warn';
  for(let index=0;index<8;index++){setCanValue(`can-wheel-torque-${index}`,validCanValue(feedback?.motor_torque_nm,feedback?.motor_torque_valid,index,1,' Nm'));setCanValue(`can-wheel-speed-${index}`,validCanValue(feedback?.motor_speed_rpm,feedback?.motor_speed_valid,index,0,' rpm'));setCanValue(`can-wheel-brake-${index}`,validCanValue(feedback?.brake_pressure_bar,feedback?.brake_valid,index,1,' bar'));const motorMode=feedback?.motor_mode_valid?.[index]?feedback.motor_mode[index]:'—',brakeMode=feedback?.brake_valid?.[index]?feedback.brake_mode[index]:'—';setCanValue(`can-wheel-mode-${index}`,`M${motorMode} / B${brakeMode}`)}
  for(let index=0;index<4;index++){setCanValue(`can-steering-angle-${index}`,validCanValue(feedback?.steering_angle_deg,feedback?.steering_valid,index,1,'°'));setCanValue(`can-steering-mode-${index}`,feedback?.steering_valid?.[index]?String(feedback.steering_mode[index]):'—')}
}
function vcuGearLabel(value){const labels={1:'N',2:'R',3:'D'};return labels[Number(value)]||`不支持(${String(value)})`}
function vcuEpbLabel(value){const labels={0:'保持',1:'释放',2:'已拉起'};return labels[Number(value)]||`异常(${String(value)})`}
function vcuSwitchLabel(value,activeLabel,inactiveLabel){const number=Number(value);if(number===1)return activeLabel;if(number===0)return inactiveLabel;return`异常(${String(value)})`}
function vcuAdapterReady(status,explicit){return controlLogic.adapterReady(status,explicit)}
function vcuDrivingReady(){return controlProfileState.acknowledged&&controlLogic.drivingReady(vcuHandshake)}
function diagnoseVcuHandshake(channelOpen){
  if(!channelOpen)return{level:'warn',text:'连接步骤未完成：控制 DataChannel 尚未连接。'};
  if(vcuHandshake.handshake_revoked){const vmc=vcuHandshake.vmc_fault_code_valid?String(vcuHandshake.vmc_fault_code):'未知',epb=Array.isArray(vcuHandshake.epb_status)?vcuHandshake.epb_status.map((value,index)=>vcuHandshake.epb_valid?.[index]?vcuEpbLabel(value):'—').join('/'):'未知';return{level:'critical',text:`握手已被 VCU 撤销（状态 5→${String(vcuHandshake.revoked_handshake_status??vcuHandshake.handshake_status??3)}）；VMC 故障码 ${vmc}，电子驻车 ${epb}。车端正在安全退出，完成后请从页面重新申请 VCU 握手。`};}
  if(!controlProfileState.acknowledged)return{level:'warn',text:controlProfileState.pendingRequestSeq?`连接步骤未完成：等待车端确认会话控制参数序号 ${controlProfileState.pendingRequestSeq}。`:'连接步骤未完成：尚未发送或确认会话控制参数。'};
  if(!vcuHandshake.supported){
    if(vcuHandshake.state==='unsupported')return{level:'ok',text:'当前适配器不需要 VCU 平行驾驶握手。'};
    return{level:'warn',text:'连接步骤未完成：尚未收到车端 VCU 状态。'};
  }
  const stateName=vcuHandshake.state||'unavailable';
  if(stateName==='closed')return{level:'critical',text:'连接步骤失败：车端 VCU 适配器已关闭，请检查 CAN bridge。'};
  if(stateName==='fault')return{level:'critical',text:'运行阶段失败：VCU 握手状态丢失或 CAN/I/O 故障，驾驶命令已阻止；请查看车端 VCU 日志 issue_code。'};
  if((stateName==='standby'||stateName==='disarmed')&&!vcuHandshake.requested&&!vcuHandshake.disarming&&!vcuHandshake.ready){
    if(!vcuHandshake.driver_gear_request_valid)return{level:'warn',text:'准入第 1 步失败：未收到物理挡位反馈；必须确认在 N 挡。'};
    const selector=vcuGearLabel(vcuHandshake.driver_gear_request);
    if(Number(vcuHandshake.driver_gear_request)!==1)return{level:'critical',text:`准入第 1 步失败：当前为 ${selector} 挡；只有 N 挡允许进入平行驾驶。`};
    const epbStatus=Array.isArray(vcuHandshake.epb_status)?vcuHandshake.epb_status:[];
    const epbValid=Array.isArray(vcuHandshake.epb_valid)?vcuHandshake.epb_valid:[];
    if(epbValid.length!==4||epbValid.some(value=>!value))return{level:'warn',text:'准入第 2 步失败：电子驻车反馈不完整；需确认四路电子驻车均已拉起。'};
    if(epbStatus.length!==4||epbStatus.some(value=>Number(value)!==2))return{level:'critical',text:`准入第 2 步失败：电子驻车未全部拉起；当前 ${epbStatus.map(vcuEpbLabel).join('/')}。`};
    if(!vcuHandshake.speed_valid)return{level:'warn',text:'准入第 3 步失败：未收到有效车速反馈；必须确认车辆静止。'};
    if(Math.abs(Number(vcuHandshake.speed_mps))>0.1)return{level:'critical',text:`准入第 3 步失败：当前车速 ${Number(vcuHandshake.speed_mps).toFixed(2)} m/s，高于 0.10 m/s。`};
    if(!vcuHandshake.handshake_valid)return{level:'warn',text:'准入第 4 步失败：未收到 VCU 握手状态；需 VCU 处于人工状态 3。'};
    if(Number(vcuHandshake.handshake_status)!==3)return{level:'critical',text:`准入第 4 步失败：VCU 当前状态 ${String(vcuHandshake.handshake_status)}，需要人工状态 3。`};
    if(!vcuHandshake.parking_ready)return{level:'warn',text:'准入第 5 步失败：N 挡、电子驻车、零速和人工状态值已满足，但反馈已过期；请检查最近 500 ms 的 CAN 更新。'};
    return{level:'ok',text:'准入检查通过：N 挡、电子驻车已拉起、车辆零速、VCU 人工状态及反馈新鲜度均满足。'};
  }
  if(stateName==='standby'&&vcuHandshake.requested){
    return{level:'warn',text:'握手请求已发送：等待车端确认并进入启动第 1/5 步。'};
  }
  const stages={
    initial:'启动第 1/5 步：正在发送 5 个周期低握手帧。',
    wait_parallel_handshake:`启动第 2/5 步未完成：复用智驾握手，等待 VCU 状态 5；当前 ${vcuHandshake.handshake_valid?String(vcuHandshake.handshake_status):'无有效反馈'}。`,
    wait_parking_brake_released:`启动第 3/5 步未完成：等待四路电子驻车释放反馈为 1；当前 ${Array.isArray(vcuHandshake.epb_status)?vcuHandshake.epb_status.map(vcuEpbLabel).join('/'):'无有效反馈'}。`,
    wait_gear:'启动第 4/5 步未完成：等待 N/R/D 目标挡位闭环反馈。',
    wait_actuator_modes:'启动第 5/5 步未完成：等待 MCU/EPS/EHB 全部进入线控模式。',
    ready:'启动完成：平行驾驶已就绪，可以发送驾驶命令。',
    disarm_torque:'退出第 1/5 步未完成：等待八路驱动扭矩归零。',
    disarm_stop:'退出第 2/5 步未完成：正在制动并等待车辆零速。',
    disarm_neutral:'退出第 3/5 步未完成：等待挡位回到 N。',
    disarm_parking_brake:'退出第 4/5 步未完成：等待四路电子驻车全部拉起。',
    disarm_manual:'退出第 5/5 步未完成：等待 VCU 回到人工状态 3。'
  };
  return{level:stateName==='ready'?'ok':'warn',text:stages[stateName]||`VCU 状态无法识别：${stateName}。`};
}
function renderVcuHandshake(){const labels={unavailable:'等待车端状态',unsupported:'当前适配器不支持 VCU 握手',closed:'车端适配器已关闭',standby:'待机（未请求）',initial:'启动 1/5 · 低握手帧',wait_parallel_handshake:'启动 2/5 · 智驾状态 5',wait_parking_brake_released:'启动 3/5 · 电子驻车释放',wait_gear:'启动 4/5 · 挡位闭环',wait_actuator_modes:'启动 5/5 · 执行器模式',ready:'握手成功（平行驾驶）',disarm_torque:'退出 1/5 · 扭矩归零',disarm_stop:'退出 2/5 · 车辆零速',disarm_neutral:'退出 3/5 · N 挡',disarm_parking_brake:'退出 4/5 · 电子驻车',disarm_manual:'退出 5/5 · 人工状态 3',disarmed:'已安全断开',fault:'VCU 通讯故障'};const channelOpen=Boolean(controlChannel&&controlChannel.readyState==='open'),supported=Boolean(vcuHandshake.supported),ready=Boolean(vcuHandshake.ready),disarming=Boolean(vcuHandshake.disarming),requested=Boolean(vcuHandshake.requested),adapterReady=vcuHandshake.adapter_ready===true,diagnostic=diagnoseVcuHandshake(channelOpen),vmcSuffix=vcuHandshake.vmc_fault_code_valid&&Number(vcuHandshake.vmc_fault_code)!==0?` · VMC ${vcuHandshake.vmc_fault_code}`:'';vcuStatus.textContent=(labels[vcuHandshake.state]||vcuHandshake.state||'未知')+vmcSuffix;vcuStatus.className=ready&&adapterReady&&controlProfileState.acknowledged?'ok':(diagnostic.level==='critical'?'critical':'warn');vcuGate.textContent=diagnostic.text;vcuGate.className=`gate-copy ${diagnostic.level}`;const selector=vcuHandshake.driver_gear_request_valid?vcuGearLabel(vcuHandshake.driver_gear_request):'未知',speed=vcuHandshake.speed_valid?`${Number(vcuHandshake.speed_mps).toFixed(2)} m/s`:'未知',manual=vcuHandshake.handshake_valid?String(vcuHandshake.handshake_status):'未知',epb=Array.isArray(vcuHandshake.epb_status)?vcuHandshake.epb_status.map(vcuEpbLabel).join('/'):'未知',vmc=vcuHandshake.vmc_fault_code_valid?String(vcuHandshake.vmc_fault_code):'未知',parkingSwitch=vcuHandshake.parking_brake_switch_valid?vcuSwitchLabel(vcuHandshake.parking_brake_switch,'已拉起','已松开'):'未知',brakePedal=vcuHandshake.brake_pedal_switch_valid?vcuSwitchLabel(vcuHandshake.brake_pedal_switch,'已踩下','已松开'):'未知';vcuGate.title=`控制链路 ${channelOpen?'已连接':'未连接'} · 参数 ${controlProfileState.acknowledged?'已确认':'未确认'} · 选择器 ${selector} · 车速 ${speed} · 电子驻车 ${epb} · VCU状态 ${manual} · VMC故障码 ${vmc} · 物理手刹 ${parkingSwitch} · 制动踏板 ${brakePedal}`;vcuConnectButton.disabled=!channelOpen||!controlProfileState.acknowledged||!adapterReady||!supported||!vcuHandshake.parking_ready||requested||ready||disarming;vcuDisconnectButton.disabled=!channelOpen||!adapterReady||!supported||(!requested&&!ready&&!disarming)}
function renderMonitoring(){const estopPresentation=controlLogic.deriveEstopPresentation(estopLatched,vehicleTelemetry?.estop===true,vehicleTelemetry?.stop_source,vehicleTelemetry?.stop_reason);renderEstopRequest(estopPresentation);renderVcuHandshake();renderCanFeedback();renderControlState();if(!authenticated){monitorPanel.hidden=true;return}monitorPanel.hidden=false;const runtime=latestRuntimeStatus||{},metrics=latestMetrics||{streams:[]},vehicles=runtime.authorized_vehicles||[],selected=vehicles.find(v=>v.vehicle_id===(runtime.vehicle_id||vehicleSelect.value));setMetric('metric-vehicle',selected?(selected.online?`${selected.vehicle_id} 在线`:`${selected.vehicle_id} 离线`):'未知',selected?.online?'ok':'warn');setMetric('metric-session',runtime.connected?`${runtime.session_id||'活动'} · ${metrics.connection_state||'等待媒体'}`:'未连接',runtime.connected?'ok':'warn');const authority=runtime.connected&&!controlAuthorityLost;setMetric('metric-authority',authority?'已获得':'无',authority?'ok':(controlAuthorityLost?'critical':'warn'));const codec=metrics.codec||mediaStatus.codec||'',backend=metrics.backend||mediaStatus.backend||'';setMetric('metric-video',codec||backend?`${codec||'未知'} / ${backend||'未知'}`:'等待媒体',codec?'ok':'warn');setMetric('metric-rtt',formatMetric(metrics.control_rtt_ms,1,' ms'),Number(metrics.control_rtt_ms)>200?'critical':(Number.isFinite(Number(metrics.control_rtt_ms))?'ok':'warn'));setMetric('metric-network',metrics.connection_method||'未知',metrics.connection_method==='TURN'?'warn':(metrics.connection_method&&metrics.connection_method!=='unknown'?'ok':'warn'));const turnConfigured=Boolean(metrics.turn_configured??hasTurnServer());setMetric('metric-turn',metrics.turn_in_use?'正在中继':(turnConfigured?'已配置，未使用':'未配置'),metrics.turn_in_use?'warn':(turnConfigured?'ok':'warn'));const sync=runtime.time_sync||metrics.time_sync||{},timeTrusted=runtime.signaling_available!==false&&Boolean(sync.synchronized)&&Number(sync.uncertainty_ms)<=consoleConfig.max_time_sync_uncertainty_ms;setMetric('metric-time',timeTrusted?`可信 ±${sync.uncertainty_ms} ms`:`不可信${Number.isFinite(Number(sync.uncertainty_ms))?` ±${sync.uncertainty_ms} ms`:''}`,timeTrusted?'ok':'critical');streamMetrics.replaceChildren();const streams=metrics.streams||[];if(!streams.length){const row=document.createElement('tr');const cell=document.createElement('td');cell.colSpan=5;cell.className='muted';cell.textContent='等待视频轨道';row.appendChild(cell);streamMetrics.appendChild(row)}for(const stream of streams){const row=document.createElement('tr');const loss=Number(stream.packet_loss_percent||0),fps=Number(stream.fps||0),latency=Number(stream.estimated_end_to_end_latency_ms||0);for(const [text,level] of [[stream.camera_id||stream.mid||'unknown',''],[formatMetric(fps,1),fps<20?'critical':'ok'],[formatMetric(stream.bitrate_kbps,0,' kbps'),''],[formatMetric(loss,2,'%'),loss>2?'warn':''],[formatMetric(latency,1,' ms'),latency>200?'critical':'ok']]){const cell=document.createElement('td');cell.textContent=text;if(level)cell.className=level;row.appendChild(cell)}streamMetrics.appendChild(row)}const alerts=[];let severity='';if(estopPresentation.visible){alerts.push(estopPresentation.alert);severity=estopPresentation.severity}if(controlAuthorityLost){alerts.push('控制权或信令已丢失，当前页面不会继续发送驾驶命令');severity='critical'}else if(runtime.connected&&(!controlChannel||controlChannel.readyState!=='open')){alerts.push('控制 DataChannel 尚未就绪');if(!severity)severity='warn'}if(vcuHandshake.supported&&!vcuHandshake.ready){const diagnostic=diagnoseVcuHandshake(Boolean(controlChannel&&controlChannel.readyState==='open'));alerts.push(diagnostic.text);if(diagnostic.level==='critical')severity='critical';else if(!severity)severity='warn'}if(!timeTrusted){alerts.push('时间同步不可信，端到端时延只作参考');severity='critical'}for(const stream of streams){if(Number(stream.estimated_end_to_end_latency_ms)>200){alerts.push(`${stream.camera_id||'视频'} 时延超过 200 ms`);severity='critical'}if(Number(stream.fps)<20){alerts.push(`${stream.camera_id||'视频'} 低于 20 FPS`);severity='critical'}}if(!alerts.length)alerts.push(streams.length?'当前指标在目标范围内':'尚无媒体指标；控制命令不会在链路未就绪时发送');alertsPanel.textContent=alerts.join('；');alertsPanel.className=`alerts ${severity}`.trim();const alertKey=`${severity}:${alerts.join('|')}`;if(alertKey!==lastAlertKey){clientLog('control_monitor_state',{severity:severity||'ok',alerts});lastAlertKey=alertKey}}
async function refreshRuntimeStatus(){if(!authenticated)return;try{latestRuntimeStatus=await get('/api/status');if(polling&&!latestRuntimeStatus.connected){closeRealtimeSession();controlAuthorityLost=true;webrtcLabel.textContent='控制权丢失'}renderMonitoring()}catch(error){controlAuthorityLost=true;resetControlAuthorityInput();webrtcLabel.textContent='本地状态读取失败';alertsPanel.textContent='无法读取本地运行状态: '+error.message;alertsPanel.className='alerts critical'}}
function clamp(value,min,max){return Math.min(max,Math.max(min,value))}
function resetControlProfileSession(){controlProfileGeneration+=1;controlProfileState={requestedProfile:null,pendingRequestSeq:0,effectiveProfile:null,effectiveRequestSeq:0,effectiveAppliedRevision:0,acknowledged:false,reason:''};pendingControlProfileEnvelope=null;lastControlProfileSendAt=0;controlProfilePrepareInFlight=false;vehicleHardLimits={received:false}}
function effectiveControlLimits(){const profile=controlProfileState.acknowledged?controlProfileState.effectiveProfile:null;if(!profile||!vehicleHardLimits.received)return{maxThrottle:0,maxBrakePressureBar:0,serviceBrakePressureBar:0,hardBrakePressureBar:0,maxSteeringDeg:0};return{maxThrottle:controlLogic.controlProfileThrottleLimit(profile,vehicleHardLimits),maxBrakePressureBar:profile.max_brake_pressure_bar,serviceBrakePressureBar:profile.service_brake_pressure_bar,hardBrakePressureBar:profile.hard_brake_pressure_bar,maxSteeringDeg:Math.min(profile.max_steering_angle_deg,vehicleHardLimits.max_steering_angle_deg)}}
function readOnlyControlSafetyText(safety){const stages=safety.deceleration_profile.map(stage=>`${stage.after_ms}ms:${stage.brake}`).join('/');return`固定安全：upstream rate ${safety.control_rate_hz} Hz · command gap ${safety.max_command_gap_ms} ms · watchdog ${safety.degraded_timeout_ms}/${safety.control_timeout_ms} ms · decel ${stages} · speed feedback ${safety.speed_feedback_timeout_ms} ms · overspeed margin ${safety.hard_overspeed_margin_kph} km/h · gates CAN=${safety.require_can_feedback_before_control}, ESTOP reset=${safety.require_local_estop_reset}, time sync=${safety.require_time_sync} (±${safety.max_time_sync_uncertainty_ms} ms / ${safety.time_sync_interval_ms} ms / ${safety.time_sync_samples} samples) · mode ${safety.commissioning_mode}`}
function renderControlLimits(){const profile=controlProfileState.effectiveProfile;if(controlProfileState.pendingRequestSeq)controlLimitsSummary.textContent=`等待车端确认参数序号 ${controlProfileState.pendingRequestSeq}`;else if(!controlProfileState.acknowledged||!profile)controlLimitsSummary.textContent='未获得车端会话参数确认（需人工打开并发送）';else controlLimitsSummary.textContent=`目标 ${profile.target_speed_kph.toFixed(1)} km/h · 单电机 ${profile.max_motor_torque_nm.toFixed(1)} Nm · EHB ${profile.service_brake_pressure_bar.toFixed(1)}/${profile.hard_brake_pressure_bar.toFixed(1)}/${profile.max_brake_pressure_bar.toFixed(1)} bar · 转向 ≤${profile.max_steering_angle_deg.toFixed(1)}° · PID ${profile.speed_pid_kp.toFixed(2)}/${profile.speed_pid_ki.toFixed(2)}/${profile.speed_pid_kd.toFixed(2)} · 升扭 ${profile.motor_torque_rise_rate_nm_per_s.toFixed(0)} Nm/s · rev ${controlProfileState.effectiveAppliedRevision}`;controlLimitsSummary.className=controlProfileState.acknowledged&&!controlProfileState.pendingRequestSeq?'ok':'warn';controlLimitsOpen.disabled=!vehicleHardLimits.received||!controlProfileState.requestedProfile;if(vehicleHardLimits.received){const pid=vehicleHardLimits.speed_pid_limits;targetSpeedKph.max=String(vehicleHardLimits.max_target_speed_kph);maxMotorTorqueNm.max=String(vehicleHardLimits.full_scale_motor_torque_nm);maxBrakePressureBar.max=String(vehicleHardLimits.max_brake_pressure_bar);serviceBrakePressureBar.max=String(vehicleHardLimits.max_brake_pressure_bar);hardBrakePressureBar.max=String(vehicleHardLimits.max_brake_pressure_bar);maxSteeringDeg.max=String(vehicleHardLimits.max_steering_angle_deg);speedPidKp.min=String(pid.kp.min);speedPidKp.max=String(pid.kp.max);speedPidKi.min=String(pid.ki.min);speedPidKi.max=String(pid.ki.max);speedPidKd.min=String(pid.kd.min);speedPidKd.max=String(pid.kd.max);speedPidDerivativeFilterTauMs.min=String(pid.derivative_filter_tau_ms.min);speedPidDerivativeFilterTauMs.max=String(pid.derivative_filter_tau_ms.max);speedPidMaxDtMs.min=String(pid.max_dt_ms.min);speedPidMaxDtMs.max=String(pid.max_dt_ms.max);motorTorqueRiseRate.min=String(vehicleHardLimits.motor_torque_rise_rate_limits_nm_per_s.min);motorTorqueRiseRate.max=String(vehicleHardLimits.motor_torque_rise_rate_limits_nm_per_s.max)}vehicleHardLimitsLabel.textContent=vehicleHardLimits.received?`车端只读硬上限：max speed ${vehicleHardLimits.max_speed_kph.toFixed(1)} km/h × max throttle ${vehicleHardLimits.max_throttle.toFixed(3)} = 目标 ${vehicleHardLimits.max_target_speed_kph.toFixed(1)} km/h · 单电机 ${vehicleHardLimits.full_scale_motor_torque_nm.toFixed(1)} Nm · 每路 EHB 普通压力 ${vehicleHardLimits.max_brake_pressure_bar.toFixed(1)} bar · 转向 ${vehicleHardLimits.max_steering_angle_deg.toFixed(1)}° · 速度反馈超时 ${vehicleHardLimits.speed_feedback_timeout_ms} ms · 硬超速余量 ${vehicleHardLimits.hard_overspeed_margin_kph} km/h。车端 PID 默认 Kp/Ki/Kd=${vehicleHardLimits.default_speed_pid_kp}/${vehicleHardLimits.default_speed_pid_ki}/${vehicleHardLimits.default_speed_pid_kd}，τ=${vehicleHardLimits.default_speed_pid_derivative_filter_tau_ms} ms，max dt=${vehicleHardLimits.default_speed_pid_max_dt_ms} ms，升扭斜率默认 ${vehicleHardLimits.default_motor_torque_rise_rate_nm_per_s} Nm/s。${readOnlyControlSafetyText(vehicleHardLimits.read_only_control_safety)}。以上硬安全制动与 watchdog 参数不可编辑。`:'等待车端完整硬上限、PID 默认值与固定安全参数；普通驾驶保持禁用'}
function sendPendingControlProfile(force=false){if(controlAuthorityLost||!pendingControlProfileEnvelope||!controlProfileState.pendingRequestSeq||controlProfileState.acknowledged||Number(pendingControlProfileEnvelope.seq)!==Number(controlProfileState.pendingRequestSeq)||!peer||peer.connectionState!=='connected'||!controlChannel||controlChannel.readyState!=='open')return false;const now=Date.now();if(!force&&now-lastControlProfileSendAt<200)return false;controlChannel.send(JSON.stringify(pendingControlProfileEnvelope));lastControlProfileSendAt=now;return true}
async function prepareControlProfile(value,announce=true){if(controlProfilePrepareInFlight)throw Error('已有会话控制参数正在准备');if(!vehicleHardLimits.received)throw Error('尚未收到车端完整硬上限与 PID 默认值');const requested=controlLogic.normalizeControlProfile(value),bounded=controlLogic.mergeControlProfileWithHardLimits(requested,vehicleHardLimits);if(JSON.stringify(requested)!==JSON.stringify(bounded))throw Error('请求超出当前车辆硬上限');const activePeer=peer,activeChannel=controlChannel,activeProfileGeneration=controlProfileGeneration;if(!activePeer||activePeer.connectionState!=='connected'||!activeChannel||activeChannel.readyState!=='open')throw Error('控制 DataChannel 尚未连接');clearControlInput(false);controlProfilePrepareInFlight=true;try{const prepared=await post('/api/control-profile',requested),requestSeq=Number(prepared?.request?.seq);if(!Number.isSafeInteger(requestSeq)||requestSeq<=0)throw Error('控制端未生成有效参数序号');if(controlProfileGeneration!==activeProfileGeneration||controlAuthorityLost||peer!==activePeer||controlChannel!==activeChannel||activePeer.connectionState!=='connected'||activeChannel.readyState!=='open')throw Error('准备参数期间控制链路已变化');controlProfileState={...controlProfileState,requestedProfile:requested,pendingRequestSeq:requestSeq,effectiveProfile:null,effectiveRequestSeq:0,effectiveAppliedRevision:0,acknowledged:false,reason:'pending'};pendingControlProfileEnvelope=prepared.request;lastControlProfileSendAt=0;sendPendingControlProfile(true);renderControlLimits();renderMonitoring();if(announce)statusPanel.textContent=`会话控制参数序号 ${requestSeq} 已发送，等待车端确认`;return prepared}finally{if(controlProfileGeneration===activeProfileGeneration)controlProfilePrepareInFlight=false}}
function applyControlProfileStatus(value){const wasAcknowledged=controlProfileState.acknowledged,next=controlLogic.reduceControlProfileStatus(controlProfileState,value);if(!next.matched)return false;controlProfileState=next;if(!controlProfileState.pendingRequestSeq)pendingControlProfileEnvelope=null;if(!controlProfileState.acknowledged&&(wasAcknowledged||next.invalidated))resetControlAuthorityInput();if(controlProfileState.acknowledged){statusPanel.textContent=`车端已确认会话控制参数序号 ${controlProfileState.effectiveRequestSeq} / revision ${controlProfileState.effectiveAppliedRevision}`;clientLog('session_control_profile_accepted',{request_seq:controlProfileState.effectiveRequestSeq,applied_revision:controlProfileState.effectiveAppliedRevision,effective_profile:controlProfileState.effectiveProfile,reason:controlProfileState.reason})}else{statusPanel.textContent=`车端会话控制参数无效：${controlProfileState.reason}`;clientLog('session_control_profile_invalidated',{reason:controlProfileState.reason})}renderControlLimits();renderMonitoring();return true}
function controlProfileParkingReady(){const mockBench=vcuMockUnsupported()&&vcuHandshake.adapter_ready===true,parkedStandby=vcuHandshake.parking_ready===true&&(vcuHandshake.state==='standby'||vcuHandshake.state==='disarmed');return mockBench||parkedStandby}
function updateVehicleHardLimits(value){if(!value||typeof value!=='object')return;try{const hard=controlLogic.normalizeVehicleHardLimits(value);vehicleHardLimits={...hard,received:true};if(!controlProfileState.requestedProfile)controlProfileState={...controlProfileState,requestedProfile:controlLogic.controlProfileFromVehicleDefaults(driverActuationDefaults,hard)};renderControlLimits()}catch(error){resetControlProfileSession();resetControlAuthorityInput();renderControlLimits();renderMonitoring();statusPanel.textContent='车端控制参数不完整，驾驶权限已撤销';clientLog('vehicle_hard_limits_invalid',{error:error.message})}}
function openControlLimits(){const requested=controlProfileState.requestedProfile;if(!vehicleHardLimits.received||!requested)throw Error('尚未收到车端完整硬上限与 PID 默认值');targetSpeedKph.value=requested.target_speed_kph.toFixed(1);maxMotorTorqueNm.value=requested.max_motor_torque_nm.toFixed(1);maxBrakePressureBar.value=requested.max_brake_pressure_bar.toFixed(1);serviceBrakePressureBar.value=requested.service_brake_pressure_bar.toFixed(1);hardBrakePressureBar.value=requested.hard_brake_pressure_bar.toFixed(1);maxSteeringDeg.value=requested.max_steering_angle_deg.toFixed(1);speedPidKp.value=requested.speed_pid_kp;speedPidKi.value=requested.speed_pid_ki;speedPidKd.value=requested.speed_pid_kd;speedPidDerivativeFilterTauMs.value=requested.speed_pid_derivative_filter_tau_ms;speedPidMaxDtMs.value=requested.speed_pid_max_dt_ms;motorTorqueRiseRate.value=requested.motor_torque_rise_rate_nm_per_s;controlLimitsConfirm.checked=false;controlLimitsApply.disabled=true;renderControlLimits();controlLimitsDialog.showModal()}
async function applyControlLimits(){if(!controlLimitsConfirm.checked)throw Error('请先确认停车或隔离台架条件');if(!vehicleHardLimits.received)throw Error('尚未收到车端完整硬上限与 PID 默认值');if(!motorTorqueRiseRate.checkValidity())throw Error('请填写车端允许范围内的升扭斜率；0 表示取消升扭限制');const requested=controlLogic.normalizeControlProfile({profile_version:3,target_speed_kph:Number(targetSpeedKph.value),max_motor_torque_nm:Number(maxMotorTorqueNm.value),max_brake_pressure_bar:Number(maxBrakePressureBar.value),service_brake_pressure_bar:Number(serviceBrakePressureBar.value),hard_brake_pressure_bar:Number(hardBrakePressureBar.value),max_steering_angle_deg:Number(maxSteeringDeg.value),speed_pid_kp:Number(speedPidKp.value),speed_pid_ki:Number(speedPidKi.value),speed_pid_kd:Number(speedPidKd.value),speed_pid_derivative_filter_tau_ms:Number(speedPidDerivativeFilterTauMs.value),speed_pid_max_dt_ms:Number(speedPidMaxDtMs.value),motor_torque_rise_rate_nm_per_s:motorTorqueRiseRate.valueAsNumber}),prior=controlProfileState.effectiveProfile||controlProfileState.requestedProfile,pidChanged=!prior||requested.speed_pid_kp!==prior.speed_pid_kp||requested.speed_pid_ki!==prior.speed_pid_ki||requested.speed_pid_kd!==prior.speed_pid_kd||requested.speed_pid_derivative_filter_tau_ms!==prior.speed_pid_derivative_filter_tau_ms||requested.speed_pid_max_dt_ms!==prior.speed_pid_max_dt_ms||requested.motor_torque_rise_rate_nm_per_s!==prior.motor_torque_rise_rate_nm_per_s,requiresParking=!controlProfileState.effectiveProfile||pidChanged||requested.target_speed_kph>prior.target_speed_kph||requested.max_motor_torque_nm>prior.max_motor_torque_nm||requested.max_brake_pressure_bar!==prior.max_brake_pressure_bar||requested.service_brake_pressure_bar!==prior.service_brake_pressure_bar||requested.hard_brake_pressure_bar!==prior.hard_brake_pressure_bar||requested.max_steering_angle_deg!==prior.max_steering_angle_deg;if(requiresParking&&!controlProfileParkingReady())throw Error('首次应用、任一 PID 或升扭斜率修改、提高目标车速/转矩、修改转向上限或制动压力，都需要 N 挡、零速、电子驻车且 VCU 为 standby/disarmed，或隔离 mock 台架');await prepareControlProfile(requested);controlLimitsDialog.close();clientLog('driver_control_profile_requested',{requested_profile:requested,vehicle_hard_limits:vehicleHardLimits})}
function applyDeadzone(value){const magnitude=Math.abs(value),deadzone=gamepadConfig.axis_deadzone;if(magnitude<=deadzone)return 0;return Math.sign(value)*(magnitude-deadzone)/(1-deadzone)}
function applyPedalDeadzone(value){const deadzone=gamepadConfig.axis_deadzone;return value<=deadzone?0:(value-deadzone)/(1-deadzone)}
function axisValue(pad,index){return Number.isInteger(index)&&index>=0&&index<pad.axes.length&&Number.isFinite(pad.axes[index])?pad.axes[index]:null}
function buttonValue(pad,index){return Number.isInteger(index)&&index>=0&&index<pad.buttons.length?Number(pad.buttons[index].value||0):0}
function syncControlKeyState(){state=controlLogic.deriveKeyState(pressedControlKeys)}
function vcuMockUnsupported(value=vcuHandshake){return controlLogic.mockUnsupported(value)}
function updateSelectedGearFromInput(inputState){const next=controlLogic.deriveGearSelection(selectedGear,inputState,vcuHandshake);if(next.changed&&pendingGearTransition){const requestedGear=next.selectedGear;clearControlInput(false);pendingGearRequest=requestedGear;statusPanel.textContent=`${pendingGearTransition.fromGear}→${pendingGearTransition.toGear} 换挡尚未获得车端反馈；已阻止新的 ${requestedGear} 挡请求，请释放后重新操作`;return{selectedGear,pendingGearRequest,changed:false}}if(next.changed)pendingGearTransition=controlLogic.createGearTransition(selectedGear,next.selectedGear,lastControlStatusSeq,++gearTransitionGeneration);selectedGear=next.selectedGear;pendingGearRequest=next.pendingGearRequest;if(pendingGearRequest)statusPanel.textContent=`${selectedGear}→${pendingGearRequest} 换挡已阻止：需至少 3 帧且持续 200 ms 的新鲜零速反馈；请停车后释放并重新按下方向键`;return next}
function updateSelectedGearFromHeldDirections(){return updateSelectedGearFromInput(state)}
function clearControlInput(resetGear=true){controlLogic.blockAndClearKeys(pressedControlKeys,blockedControlKeys);syncControlKeyState();gamepadState.steering=0;gamepadState.throttle=0;gamepadState.brake=0;gamepadRequiresNeutral=true;if(resetGear){selectedGear='N';pendingGearRequest=null;pendingGearTransition=null}renderControlState()}
function vcuStateRequiresFreshInput(value=vcuHandshake){return controlLogic.requiresFreshInput(value)}
function vcuStateKeepsHeldInput(value=vcuHandshake){return controlLogic.keepsHeldInput(vcuEverReady,value)}
function acceptControlStatusMessage(message){const decision=controlLogic.reduceStatusSequence(lastControlStatusSeq,message?.control_status_seq);if(!decision.accepted){const sequence=Number(message?.control_status_seq);clientLog('control_status_message_dropped',{event:message?.event||'unknown',control_status_seq:Number.isFinite(sequence)?sequence:null,last_control_status_seq:lastControlStatusSeq});return false}if(decision.gap>0)clientLog('control_status_sequence_gap',{event:message?.event||'unknown',control_status_seq:decision.lastSequence,last_control_status_seq:lastControlStatusSeq,missing_status_count:decision.gap});lastControlStatusSeq=decision.lastSequence;return true}
function resetControlAuthorityInput(){vcuEverReady=false;clearControlInput()}
function updateVcuHandshakeState(value){gearChangeStationaryEvidence=controlLogic.updateGearChangeStationaryEvidence(gearChangeStationaryEvidence,value,lastControlStatusSeq,performance.now());value={...value,gear_change_stationary_confirmed:gearChangeStationaryEvidence.confirmed};const transition=controlLogic.transitionVcuState(vcuEverReady,value);vcuHandshake=value;vcuEverReady=transition.everReady;if(transition.resetInput)resetControlAuthorityInput()}
function applyVehicleSafetyState(value){const next=String(value||'');if(next==='DEGRADED'){clearControlInput(false);if(lastVehicleSafetyState!=='DEGRADED'){lastKeyboardEvent.textContent='控制命令短暂中断，输入已清除 · 请释放后重新按下';statusPanel.textContent='车端进入可恢复降级：牵引已清零；请释放控制键后重新按下';clientLog('driver_input_cleared_on_degraded',{previous_safety_state:lastVehicleSafetyState||null})}}lastVehicleSafetyState=next}
function suspendSignalingPoll(){const generation=++signalingGeneration;polling=false;if(signalingPollAbort){signalingPollAbort.abort();signalingPollAbort=null}return generation}
function closeRealtimeSession(){flushControlTrace('session_close');controlTraceScope={session_id:'',vehicle_id:''};lastHeartbeatTraceAt=null;nativeControlSessionId='';nativeControlSessionGeneration=0;lastNativeIntentSnapshot='';const generation=suspendSignalingPoll();gearRejectionInhibited=false;resetControlAuthorityInput();resetControlProfileSession();lastControlStatusSeq=0;gearChangeStationaryEvidence=controlLogic.createGearChangeStationaryEvidence();resetControlOutcomeSession();if(controlChannel)controlChannel.close();if(peer)peer.close();controlChannel=null;peer=null;vehicleTelemetry=null;lastVehicleSafetyState='';vcuHandshake={supported:false,state:'unavailable',ready:false,requested:false,disarming:false,parking_ready:false,driver_connected:false,adapter_ready:null};pendingIce=[];remoteCameraIds=[];offeredCameraByMid.clear();cameraByMid.clear();assignedCameraIds.clear();previousStats.clear();cameraGrid.replaceChildren(emptyStage);renderMonitoring();return generation}
function renderEstopRequest(presentation=controlLogic.deriveEstopPresentation(estopLatched,vehicleTelemetry?.estop===true,vehicleTelemetry?.stop_source,vehicleTelemetry?.stop_reason)){estopStatus.hidden=!presentation.visible;estopStatus.textContent=presentation.banner}
function latchEstop(source){if(estopLatched)return false;estopLatched=true;clientLog('control_estop_request_latched',{source});renderEstopRequest();renderMonitoring();return true}
function firstConnectedGamepad(){const pads=navigator.getGamepads?navigator.getGamepads():[];if(activeGamepadIndex!==null&&pads[activeGamepadIndex]?.connected)return pads[activeGamepadIndex];for(const pad of pads)if(pad?.connected){activeGamepadIndex=pad.index;return pad}activeGamepadIndex=null;return null}
function applyGamepadNeutralInterlock(authorityReady,gearRequestPending=false){const next=controlLogic.reduceGamepadNeutralInterlock({requiresNeutral:gamepadRequiresNeutral,authorityReady,throttle:gamepadState.throttle,brake:gamepadState.brake,gearRequestPending});gamepadRequiresNeutral=next.requiresNeutral;gamepadState.throttle=next.throttle;gamepadState.brake=next.brake;return next}
function sampleGamepad(){if(!gamepadConfig.enabled||document.hidden||!document.hasFocus()){gamepadState.connected=false;gamepadState.steering=0;gamepadState.throttle=0;gamepadState.brake=0;renderControlState();return}const pad=firstConnectedGamepad();if(!pad){gamepadState.connected=false;gamepadState.steering=0;gamepadState.throttle=0;gamepadState.brake=0;renderControlState();return}gamepadState.connected=true;const standard=pad.mapping==='standard';if(standard){const steering=axisValue(pad,0);let steeringValue=steering===null?0:(steering-calibration.steeringCenter)/calibration.steeringRange;if(gamepadConfig.steering_inverted)steeringValue=-steeringValue;gamepadState.steering=clamp(applyDeadzone(steeringValue),-1,1);gamepadState.throttle=clamp(applyPedalDeadzone(buttonValue(pad,7)),0,1);gamepadState.brake=clamp(applyPedalDeadzone(buttonValue(pad,6)),0,1)}else{const steering=axisValue(pad,gamepadConfig.steering_axis),throttle=axisValue(pad,gamepadConfig.throttle_axis),brake=axisValue(pad,gamepadConfig.brake_axis);if(steering===null||throttle===null||brake===null){gamepadState.steering=0;gamepadState.throttle=0;gamepadState.brake=0;renderControlState();return}let steeringValue=(steering-calibration.steeringCenter)/calibration.steeringRange;if(gamepadConfig.steering_inverted)steeringValue=-steeringValue;gamepadState.steering=clamp(applyDeadzone(steeringValue),-1,1);const throttleDelta=gamepadConfig.throttle_inverted?calibration.throttleRest-throttle:throttle-calibration.throttleRest;const brakeDelta=gamepadConfig.brake_inverted?calibration.brakeRest-brake:brake-calibration.brakeRest;gamepadState.throttle=clamp(applyPedalDeadzone(throttleDelta/calibration.throttleRange),0,1);gamepadState.brake=clamp(applyPedalDeadzone(brakeDelta/calibration.brakeRange),0,1)}const gamepadAuthorityReady=vcuEverReady||vcuMockUnsupported();applyGamepadNeutralInterlock(gamepadAuthorityReady);if(gamepadState.throttle>0&&selectedGear==='N'){const nextGear=updateSelectedGearFromInput({up:true,down:false});if(nextGear.pendingGearRequest)applyGamepadNeutralInterlock(gamepadAuthorityReady,true)}const estopPressed=buttonValue(pad,gamepadConfig.estop_button)>=0.5;if(estopPressed){if(!gamepadEstopPressedAt)gamepadEstopPressedAt=performance.now();if(performance.now()-gamepadEstopPressedAt>=consoleConfig.estop_hold_ms&&latchEstop('Gamepad'))send({estop:true},false).catch(console.error)}else gamepadEstopPressedAt=0;renderControlState()}
function currentControl(extra={}){const control=controlLogic.deriveControl({keyState:state,gamepad:gamepadState,selectedGear,limits:effectiveControlLimits(),steeringFullScaleDeg:limitConfig.steering_full_scale_deg,estop:estopLatched||Boolean(extra.estop)});if(pendingGearTransition&&!control.estop)control.throttle=0;return control}
function setControlReadout(name,text,active=false){for(const element of [controlReadouts[name],operatorControlReadouts[name]]){element.textContent=text;element.parentElement?.classList.toggle('active',active)}}
function renderControlState(){
  const control=currentControl();
  for(const [name,element] of Object.entries(keyIndicators)){const active=Boolean(state[name]);element.classList.toggle('active',active);element.setAttribute('aria-pressed',String(active))}
  setControlReadout('gear',control.gear);setControlReadout('steering',control.steering.toFixed(2),Math.abs(control.steering)>0.001);setControlReadout('throttle',control.throttle.toFixed(2),control.throttle>0.001);setControlReadout('brake',control.brake.toFixed(2),control.brake>0.001);
  const linkReady=polling&&peer?.connectionState==='connected'&&controlChannel?.readyState==='open',vcuReady=vcuDrivingReady(),ready=linkReady&&vcuReady,retainedWait=vcuHandshake.adapter_ready===true&&vcuEverReady&&vcuHandshake.state==='wait_gear'?'换挡闭环中（输入保持）':(vcuHandshake.adapter_ready===true&&vcuEverReady&&vcuHandshake.state==='wait_actuator_modes'?'执行器闭环中（输入保持）':''),terminalState=vcuStateRequiresFreshInput(vcuHandshake),freshReadyRequired=linkReady&&vcuHandshake.ready&&!vcuEverReady;
  inputReadiness.textContent=estopLatched?'急停请求锁定':(gearRejectionInhibited?'换挡拒绝状态不确定，普通控制已冻结':(pendingGearRequest?`等待有效零速后重新选择 ${pendingGearRequest}`:(ready?'控制已就绪':(retainedWait||(terminalState?'VCU 故障/退出，输入已清除':(freshReadyRequired?'输入已清除，等待新鲜 VCU Ready':(linkReady?'等待 VCU 握手':(polling?'等待控制链路':'等待连接'))))))));
  inputReadiness.className=`status-chip ${estopLatched||gearRejectionInhibited||terminalState?'critical':(ready?'ok':'warn')}`;
}
function renderVehicles(vehicles=[]){const previous=vehicleSelect.value,currentVehicle=latestRuntimeStatus.connected?latestRuntimeStatus.vehicle_id:'';vehicleSelect.replaceChildren();let firstSelectable='',previousAvailable=false;const labels={online:'在线可控',offline:'离线',active:'控制中',reserved:'已预留',connecting:'连接中',revoked:'已撤销'};for(const vehicle of vehicles){const option=document.createElement('option'),current=vehicle.vehicle_id===currentVehicle,selectable=vehicle.controllable||current;option.value=vehicle.vehicle_id;option.textContent=`${vehicle.vehicle_id} · ${current?'当前会话':(labels[vehicle.state]||vehicle.state)}`;option.disabled=!selectable;if(selectable&&!firstSelectable)firstSelectable=vehicle.vehicle_id;if(selectable&&vehicle.vehicle_id===previous)previousAvailable=true;vehicleSelect.appendChild(option)}vehicleSelect.value=previousAvailable?previous:firstSelectable;connectButton.disabled=connecting||!vehicleSelect.value}
function renderAuthExpiry(expiresAt){authExpiry.textContent=expiresAt?`认证有效至 ${new Date(expiresAt).toLocaleString()}`:''}
function requireLogin(message){closeRealtimeSession();authenticated=false;controlAuthorityLost=false;connectButton.textContent='连接所选车辆';renderAuthExpiry(0);sessionPanel.hidden=true;vcuPanel.hidden=true;monitorPanel.hidden=true;loginPanel.hidden=false;statusPanel.textContent=message;clientLog('driver_reauthentication_required',{reason:message})}
function handleVehicleRefreshError(error){if(error.status===401){requireLogin('登录已失效，请重新认证: '+error.message);return}statusPanel.textContent='车辆状态刷新失败，当前会话已保留: '+error.message;clientLog('vehicle_list_refresh_failed',{error:error.message})}
async function login(){const password=passwordInput.value;if(!password)throw Error('请输入驾驶员密码');passwordInput.value='';const result=await post('/api/login',{password});authenticated=true;controlAuthorityLost=false;webrtcLabel.textContent='未连接';loginPanel.hidden=true;sessionPanel.hidden=false;vcuPanel.hidden=false;renderVehicles(result.vehicles||[]);renderAuthExpiry(result.token_expires_at_utc_ms);sampleGamepad();latestRuntimeStatus=await get('/api/status');renderMonitoring();statusPanel.textContent=`已登录 ${result.driver_id}，请选择在线车辆`;clientLog('driver_login_succeeded',{driver_id:result.driver_id,authorized_vehicle_count:(result.vehicles||[]).length})}
async function refreshVehicles(){if(!authenticated)return;const result=await get('/api/vehicles');renderVehicles(result.vehicles||[]);renderAuthExpiry(result.token_expires_at_utc_ms);if(result.signaling_available===false){controlAuthorityLost=true;resetControlAuthorityInput();connectButton.disabled=true;statusPanel.textContent='信令服务暂时不可用；车辆列表为安全快照，禁止建立控制会话';renderMonitoring();return}if(result.signaling_restart_recovered){closeRealtimeSession();controlAuthorityLost=true;latestRuntimeStatus=await get('/api/status');connectButton.textContent='连接所选车辆';webrtcLabel.textContent='服务已恢复，需重新建立控制会话';statusPanel.textContent='信令服务已重启，驾驶员身份已自动恢复；旧控制权未恢复，请重新选择车辆';clientLog('signaling_restart_recovered',{previous_service_instance_id:result.previous_service_instance_id,service_instance_id:result.service_instance_id,control_authority_recovered:false});renderMonitoring()}}
function sendVcuHandshakeCommand(action){if(!controlChannel||controlChannel.readyState!=='open')throw Error('控制 DataChannel 尚未连接');if(action==='connect'&&!controlProfileState.acknowledged)throw Error('会话控制参数尚未获得车端确认');if(vcuHandshake.adapter_ready!==true)throw Error('VCU 适配器尚未就绪');if(!['connect','disconnect'].includes(action))throw Error('VCU 握手命令非法');if(action==='disconnect'){resetControlAuthorityInput();vcuHandshake={...vcuHandshake,ready:false,disarming:true}}else{gearRejectionInhibited=false;vcuHandshake={...vcuHandshake,requested:true}}renderMonitoring();controlChannel.send(JSON.stringify({event:'vcu_handshake_command',action,sent_at_utc_ms:Date.now()}));clientLog('driver_vcu_handshake_command',{action});statusPanel.textContent=action==='connect'?'已请求开始 VCU 平行驾驶握手':'已请求安全断开 VCU 握手'}
function nativeIntentEnvelope(outgoing){
  const normalized={gear:String(outgoing.gear||'N'),steering:Number(outgoing.steering||0),throttle:Number(outgoing.throttle||0),brake:Number(outgoing.brake||0),estop:Boolean(outgoing.estop)};
  const snapshot=JSON.stringify(normalized);
  if(!nativeIntentSeq||snapshot!==lastNativeIntentSnapshot){nativeIntentSeq++;lastNativeIntentSnapshot=snapshot}
  return{session_id:nativeControlSessionId,session_generation:nativeControlSessionGeneration,ui_instance_id:uiInstanceId,intent_seq:nativeIntentSeq,...normalized}
}
async function writeControlIntent(extra,announceUnavailable){
  const activePeer=peer,activeChannel=controlChannel,estopRequested=estopLatched||Boolean(extra.estop);
  if(controlAuthorityLost){
    resetControlAuthorityInput();
    if(announceUnavailable)webrtcLabel.textContent='控制权丢失';
    return{sent:false,reason:'control_authority_lost'}
  }
  let blockReason='';
  if(!activePeer||activePeer.connectionState!=='connected'||!activeChannel||activeChannel.readyState!=='open'){
    blockReason='control_link_unavailable';
    if(announceUnavailable)webrtcLabel.textContent='控制链路中断'
  }else if(gearRejectionInhibited&&!estopRequested){
    blockReason='gear_rejection_unresolved'
  }else if(!estopRequested&&!controlProfileState.acknowledged){
    blockReason='control_profile_not_acknowledged';
    if(announceUnavailable)statusPanel.textContent='会话控制参数尚未获得车端确认，驾驶命令已阻止'
  }
  const retainedWait=vcuHandshake.adapter_ready===true&&vcuStateKeepsHeldInput(vcuHandshake),gearTransitionPending=Boolean(pendingGearTransition);
  if(!blockReason&&estopRequested&&vcuHandshake.adapter_ready===false){
    blockReason='vcu_adapter_unavailable';
    if(announceUnavailable)statusPanel.textContent='VCU 适配器明确不可用，远程急停未发送；请使用车辆物理急停'
  }else if(!blockReason&&!vcuDrivingReady()&&!estopRequested&&!retainedWait){
    blockReason='vcu_handshake_not_ready';
    if(announceUnavailable)statusPanel.textContent='VCU 平行驾驶握手未成功，驾驶命令已阻止'
  }
  if(blockReason)clearControlInput(false);
  const outgoing=currentControl(blockReason?{}:extra);
  if((retainedWait||gearTransitionPending)&&!estopRequested)outgoing.throttle=0;
  const outgoingSnapshot=controlLogic.controlSnapshot(outgoing);
  const transitionGeneration=!blockReason&&!estopRequested&&pendingGearTransition?pendingGearTransition.generation:0;
  const intent=nativeIntentEnvelope(outgoing);
  const controller=new AbortController(),deadlineMs=Math.max(75,Math.min(150,Math.floor(Number(consoleConfig.intent_lease_ms||200)*0.75))),timer=setTimeout(()=>controller.abort(),deadlineMs);
  activeControlPrepareAbort=controller;activeControlPrepareIsEstop=estopRequested;
  let accepted;
  try{
    accepted=await post('/api/control-intent',intent,controller.signal)
  }catch(error){
    if(intent.session_id!==nativeControlSessionId||intent.session_generation!==nativeControlSessionGeneration)return{sent:false,reason:'stale_control_intent_response'};
    if(error.name==='AbortError'){
      clearControlInput(false);
      if(activeControlPreparePreemptedByEstop&&!estopRequested)return{sent:false,reason:'control_intent_preempted_by_estop'};
      clientLog('control_intent_update_timeout',{intent_seq:intent.intent_seq,deadline_ms:deadlineMs});
      return{sent:false,reason:'control_intent_update_timeout'}
    }
    if([401,403,409].includes(error.status)){controlAuthorityLost=true;resetControlAuthorityInput();resetControlProfileSession()}
    throw error
  }finally{clearTimeout(timer);if(activeControlPrepareAbort===controller){activeControlPrepareAbort=null;activeControlPrepareIsEstop=false;activeControlPreparePreemptedByEstop=false}}
  if(intent.session_id!==nativeControlSessionId||intent.session_generation!==nativeControlSessionGeneration)return{sent:false,reason:'stale_control_intent_response'};
  if(!accepted.accepted){
    if(accepted.reason==='fresh_neutral_required'){
      clearControlInput(false);
      lastNativeIntentSnapshot='';
    }
    return{...accepted,sent:false,reason:accepted.reason||'control_intent_rejected'}
  }
  if(transitionGeneration){
    pendingGearTransition=controlLogic.recordForwardedGearCommand(pendingGearTransition,transitionGeneration,accepted.intent_seq,outgoingSnapshot.gear)
  }
  if(!accepted.duplicate)clientLog('control_intent_accepted',{intent_seq:accepted.intent_seq,transport:accepted.transport,gear:outgoingSnapshot.gear,estop:estopRequested});
  return{...accepted,sent:true,blocked_reason:blockReason||null}
}
function reportControlQueueError(error){if(controlTraceEnabled)noteScopedControlTraceCounter('queue_unhandled_error_count',error&&typeof error==='object'?(controlTraceErrorScopes.get(error)||controlTraceScope):controlTraceScope);console.error(error)}
const controlWriteQueue=controlLogic.createLatestControlWriteQueue(writeControlIntent,reportControlQueueError,()=>{if(activeControlPrepareAbort&&!activeControlPrepareIsEstop){activeControlPreparePreemptedByEstop=true;activeControlPrepareAbort.abort()}});
function enqueueIntentRefresh(){return controlWriteQueue.enqueueHeartbeat()}
async function send(extra={},announceUnavailable=true){if(controlTraceEnabled)controlTraceSummary.explicit_send_count++;return controlWriteQueue.send(extra,announceUnavailable)}
async function refreshControlIntent(){const now=performance.now();if(!polling){lastHeartbeatTraceAt=null;return}noteIntentRefresh(now);sampleGamepad();sendPendingControlProfile();const enqueued=enqueueIntentRefresh();if(controlTraceEnabled){if(enqueued)controlTraceSummary.heartbeat_enqueued_count++;else controlTraceSummary.heartbeat_coalesced_count++}}
function advertisedCodecs(){const caps=RTCRtpReceiver.getCapabilities&&RTCRtpReceiver.getCapabilities('video');const found=new Set(['h264']);for(const c of (caps&&caps.codecs)||[]){const m=(c.mimeType||'').toLowerCase();if(m.includes('h265')||m.includes('hevc'))found.add('h265');if(m.includes('h264')||m.includes('avc'))found.add('h264')}return [...found]}
async function connect(){if(connecting)return;const target=vehicleSelect.value;if(!target)throw Error('没有可连接的在线车辆');const fromVehicle=latestRuntimeStatus.connected?latestRuntimeStatus.vehicle_id:'';if(polling&&fromVehicle===target){statusPanel.textContent=`车辆 ${target} 已处于当前会话`;return}const changingVehicle=Boolean(fromVehicle)&&fromVehicle!==target;const reconnecting=Boolean(fromVehicle)&&fromVehicle===target;const hadRealtime=polling;let suspendedGeneration=signalingGeneration;if((changingVehicle||reconnecting)&&hadRealtime){suspendedGeneration=suspendSignalingPoll();clearControlInput()}connecting=true;connectButton.disabled=true;if(changingVehicle){webrtcLabel.textContent='正在安全切换车辆';statusPanel.textContent=`正在验证 ${target}，成功后释放 ${fromVehicle}`;clientLog('driver_vehicle_switch_started',{from_vehicle_id:fromVehicle,to_vehicle_id:target})}let session=null,generation=signalingGeneration;try{session=await post('/api/connect',{vehicle_id:target});generation=closeRealtimeSession();nativeControlSessionId=String(session.session_id||'');nativeControlSessionGeneration=Number(session.control_session_generation);if(!nativeControlSessionId||!Number.isSafeInteger(nativeControlSessionGeneration)||nativeControlSessionGeneration<=0)throw Error('原生控制会话代次无效');setControlTraceScope(session.session_id,session.vehicle_id);controlAuthorityLost=true;const ice=await post('/api/webrtc/ice-servers');iceServers=ice.ice_servers||[];await post('/api/webrtc/capabilities',{codecs:advertisedCodecs()});polling=true;controlAuthorityLost=false;latestRuntimeStatus=await get('/api/status');webrtcLabel.textContent='等待车端媒体';statusPanel.textContent=`会话 ${session.session_id} · ${session.vehicle_id}`;connectButton.textContent='切换所选车辆';document.querySelector('main').focus();renderMonitoring();clientLog(changingVehicle?'driver_vehicle_switched':(reconnecting?'driver_session_reconnected':'driver_session_connected'),{from_vehicle_id:fromVehicle||undefined,session_id:session.session_id,vehicle_id:session.vehicle_id});pollSignaling(generation)}catch(error){if(session){flushControlTrace('connect_setup_failed');controlTraceScope={session_id:'',vehicle_id:''};nativeControlSessionId='';nativeControlSessionGeneration=0;lastNativeIntentSnapshot='';await post('/api/end-session',{reason:'driver_connect_setup_failed'}).catch(()=>{})}latestRuntimeStatus=await get('/api/status').catch(()=>({connected:false}));const retained=Boolean(!session&&latestRuntimeStatus.connected&&hadRealtime);if(retained){polling=true;controlAuthorityLost=false;webrtcLabel.textContent=controlChannel&&controlChannel.readyState==='open'?'控制链路已连接':'当前会话已保留';statusPanel.textContent=`切换失败，当前会话已保留: ${error.message}`;clientLog('driver_vehicle_switch_rejected',{from_vehicle_id:fromVehicle,to_vehicle_id:target,error:error.message});pollSignaling(suspendedGeneration)}else{controlAuthorityLost=Boolean(latestRuntimeStatus.connected)}connectButton.textContent=latestRuntimeStatus.connected?'切换所选车辆':'连接所选车辆';renderMonitoring();if(!retained)throw error}finally{connecting=false;connectButton.disabled=!vehicleSelect.value}}
async function logout(){const estopConfirmed=vehicleTelemetry?.estop===true;closeRealtimeSession();controlAuthorityLost=true;webrtcLabel.textContent='正在释放控制权';await post('/api/disconnect',{reason:'driver_safe_logout'});authenticated=false;controlAuthorityLost=false;connectButton.textContent='连接所选车辆';renderAuthExpiry(0);sessionPanel.hidden=true;vcuPanel.hidden=true;canFeedbackPanel.hidden=true;monitorPanel.hidden=true;loginPanel.hidden=false;webrtcLabel.textContent='未连接';statusPanel.textContent=estopLatched?(estopConfirmed?'已安全退出；车辆急停已确认，仍需本地确认复位':'已安全退出；急停请求未获车端确认，请在车辆本地核实'):'已安全退出';clientLog('driver_safe_logout',{estop_request_latched:estopLatched,estop_confirmed:estopConfirmed})}
addEventListener('pagehide',()=>{flushControlTrace('pagehide');closeRealtimeSession();if(authenticated)fetch('/api/disconnect',{method:'POST',headers:{'content-type':'application/json','x-mine-teleop-page-capability':pageCapability},body:JSON.stringify({reason:'browser_page_closed'}),keepalive:true}).catch(()=>{})});
function neutralizeInput(){clearControlInput(false);send({},false).catch(console.error)}
addEventListener('blur',neutralizeInput);document.addEventListener('visibilitychange',()=>{if(document.hidden)neutralizeInput()});
document.querySelector('#login').onclick=()=>login().catch(e=>{statusPanel.textContent='登录失败: '+e.message});
passwordInput.addEventListener('keydown',e=>{if(e.key==='Enter')login().catch(error=>{statusPanel.textContent='登录失败: '+error.message})});
document.querySelector('#connect').onclick=()=>connect().catch(e=>{webrtcLabel.textContent='连接失败';statusPanel.textContent=e.message});
document.querySelector('#logout').onclick=()=>logout().catch(e=>{statusPanel.textContent='退出失败: '+e.message});
document.querySelector('#estop').onclick=()=>{latchEstop('页面按钮');send({estop:true}).catch(alert)};
controlLimitsOpen.onclick=openControlLimits;
controlLimitsConfirm.onchange=()=>{controlLimitsApply.disabled=!controlLimitsConfirm.checked};
controlLimitsApply.onclick=()=>applyControlLimits().catch(error=>{statusPanel.textContent='限幅设置失败: '+error.message});
controlLimitsCancel.onclick=()=>controlLimitsDialog.close();
vcuConnectButton.onclick=()=>{try{sendVcuHandshakeCommand('connect')}catch(error){statusPanel.textContent='开始握手失败: '+error.message}};
vcuDisconnectButton.onclick=()=>{try{sendVcuHandshakeCommand('disconnect')}catch(error){statusPanel.textContent='断开握手失败: '+error.message}};
const keyNames={left:'左转',right:'右转',up:'前进',down:'倒车',service_brake:'缓刹',hard_brake:'急刹'};
renderControlLimits();
renderControlState();
addEventListener('gamepadconnected',e=>{activeGamepadIndex=e.gamepad.index;sampleGamepad();clientLog('gamepad_connected',{id:e.gamepad.id,mapping:e.gamepad.mapping,axes:e.gamepad.axes.length,buttons:e.gamepad.buttons.length})});addEventListener('gamepaddisconnected',e=>{if(activeGamepadIndex===e.gamepad.index)activeGamepadIndex=null;gamepadState.connected=false;gamepadState.steering=0;gamepadState.throttle=0;gamepadState.brake=0;gamepadRequiresNeutral=true;renderControlState();clientLog('gamepad_disconnected',{id:e.gamepad.id})});
function editingTarget(target){return ['INPUT','SELECT','TEXTAREA','BUTTON'].includes(target?.tagName)||Boolean(target?.isContentEditable)}
addEventListener('keydown',e=>{const binding=keys[e.code],estopKey=e.code==='KeyE';if(!binding&&!estopKey)return;if(editingTarget(e.target))return;e.preventDefault();if(!polling){if(binding)controlLogic.blockKey(blockedControlKeys,e.code);lastKeyboardEvent.textContent=`${estopKey?'急停':keyNames[binding]}已截获 · 等待连接`;return}if(estopKey){if(!e.repeat){lastKeyboardEvent.textContent='急停请求已锁定 · E';latchEstop('键盘 E');send({estop:true}).catch(console.error)}return}if(blockedControlKeys.has(e.code)){lastKeyboardEvent.textContent=`${keyNames[binding]}需释放后重新按下 · ${e.code}`;return}if(vcuStateRequiresFreshInput(vcuHandshake)){controlLogic.blockKey(blockedControlKeys,e.code);lastKeyboardEvent.textContent=`${keyNames[binding]}已阻止 · 等待 VCU 恢复后重新按下`;return}if(!vcuEverReady&&!vcuMockUnsupported()){controlLogic.blockKey(blockedControlKeys,e.code);lastKeyboardEvent.textContent=`${keyNames[binding]}已阻止 · 首次握手完成后请重新按下`;return}const pressed=controlLogic.pressKey(pressedControlKeys,blockedControlKeys,e.code);if(pressed.changed){syncControlKeyState();updateSelectedGearFromHeldDirections();lastKeyboardEvent.textContent=`${keyNames[binding]}按下 · ${e.code}`;renderControlState();send().catch(console.error)}});
addEventListener('keyup',e=>{const binding=keys[e.code];if(!binding)return;if(!editingTarget(e.target))e.preventDefault();const released=controlLogic.releaseKey(pressedControlKeys,blockedControlKeys,e.code);syncControlKeyState();updateSelectedGearFromHeldDirections();lastKeyboardEvent.textContent=`${keyNames[binding]}释放 · ${e.code}`;renderControlState();if(released.changed&&polling)send().catch(console.error)});
async function pollSignaling(generation){const controller=new AbortController();signalingPollAbort=controller;while(polling&&generation===signalingGeneration){try{const data=await post('/api/poll-signaling',{},controller.signal);if(generation!==signalingGeneration)break;for(const message of data.messages||[]){if(message.type==='webrtc_offer')await startFromOffer(message.payload||{});if(message.type==='ice_candidate')await addIce(message.payload||{});if(message.type==='media_status'){mediaStatus=message.payload||{lanes:[]};renderMonitoring()}}}catch(e){if(generation!==signalingGeneration||e.name==='AbortError')break;closeRealtimeSession();controlAuthorityLost=true;webrtcLabel.textContent='控制权或信令中断';statusPanel.textContent='信令轮询失败，已停止驾驶命令: '+e.message;post('/api/end-session',{reason:'signaling_poll_failed'}).catch(()=>{});clientLog('signaling_poll_failed',{error:e.message});renderMonitoring();break}await new Promise(r=>setTimeout(r,100))}if(signalingPollAbort===controller)signalingPollAbort=null}
async function addIce(candidate){if(!candidate.candidate)return;if(!peer||!peer.remoteDescription){pendingIce.push(candidate);return}await peer.addIceCandidate(candidate)}
function offeredVideoCameraIds(sdp,tracks){const mapping=new Map(),cameraIds=(tracks||[]).map(track=>track.camera_id).filter(Boolean);let cameraIndex=0;for(const section of String(sdp||'').split(/\r?\nm=/).slice(1)){if(!section.startsWith('video '))continue;const match=section.match(/(?:^|\r?\n)a=mid:([^\r\n]+)/),cameraId=cameraIds[cameraIndex++];if(match&&cameraId)mapping.set(match[1],cameraId)}return mapping}
function attach(cameraId,track){if(emptyStage.isConnected)emptyStage.remove();let box=document.getElementById('camera-'+cameraId);if(!box){box=document.createElement('article');box.id='camera-'+cameraId;box.className='camera';box.innerHTML='<span class="label"></span><video autoplay playsinline muted></video>';box.querySelector('.label').textContent=cameraId;cameraGrid.appendChild(box)}box.querySelector('video').srcObject=new MediaStream([track])}
async function startFromOffer(offer){
  if(peer)peer.close();
  controlChannel=null;
  resetControlProfileSession();
  lastControlStatusSeq=0;
  vehicleTelemetry=null;
  lastVehicleSafetyState='';
  vcuHandshake={supported:false,state:'unavailable',ready:false,requested:false,disarming:false,parking_ready:false,driver_connected:false,adapter_ready:null};
  resetControlAuthorityInput();
  cameraGrid.replaceChildren();
  pendingIce=[];
  cameraByMid.clear();
  assignedCameraIds.clear();
  previousStats.clear();
  h265FailureSamples=0;
  h265FallbackSent=false;
  remoteCameraIds=(offer.media_tracks||[]).map(t=>t.camera_id);
  offeredCameraByMid=offeredVideoCameraIds(offer.sdp,offer.media_tracks||[]);
  const nextPeer=new RTCPeerConnection({bundlePolicy:'max-bundle',iceServers,iceTransportPolicy:consoleConfig.ice_transport_policy});
  peer=nextPeer;
  webrtcLabel.textContent=`协商 ${offer.codec||''}/${offer.backend||''}`;
  nextPeer.onconnectionstatechange=()=>{
    if(peer!==nextPeer)return;
    const connectionState=nextPeer.connectionState;
    webrtcLabel.textContent=connectionState;
    if(connectionState==='disconnected'){
      lastVehicleSafetyState='';
      resetControlAuthorityInput();
      resetControlProfileSession();
      clientLog('webrtc_peer_disconnected');
    }
    if(['failed','closed'].includes(connectionState)){
      const terminalChannel=controlChannel;
      flushControlTrace(`peer_${connectionState}`);
      clientLog('webrtc_peer_terminal',{connection_state:connectionState,trace_session_id:controlTraceScope.session_id||null,trace_vehicle_id:controlTraceScope.vehicle_id||null,data_channel_ready_state:terminalChannel?.readyState||'none',buffered_amount_bytes:Math.max(0,Number(terminalChannel?.bufferedAmount)||0)});
      if(controlChannel===terminalChannel)controlChannel=null;
      lastVehicleSafetyState='';
      resetControlAuthorityInput();
      resetControlProfileSession();
    }
    if(connectionState==='connected'&&controlChannel?.readyState==='open')webrtcLabel.textContent='控制链路已连接';
    renderMonitoring();
  };
  nextPeer.onicecandidateerror=e=>clientLog('webrtc_ice_candidate_error',{endpoint:safeIceEndpoint(e.url),error_code:Number(e.errorCode||0)});
  nextPeer.ondatachannel=e=>{
    const channel=e.channel;
    if(!controlLogic.isCurrentPeer(peer,nextPeer)){channel.close();return}
    if(channel.label!=='control'||channel.protocol!=='mine-teleop-control-v1'||channel.ordered||channel.maxRetransmits!==0){
      channel.close();
      webrtcLabel.textContent='控制通道参数非法';
      clientLog('control_datachannel_rejected',{label:channel.label,protocol:channel.protocol,ordered:channel.ordered,max_retransmits:channel.maxRetransmits});
      return;
    }
    lastControlStatusSeq=0;
    controlChannel=channel;
    channel.bufferedAmountLowThreshold=1024;
    channel.onopen=()=>{
      if(!controlLogic.isCurrentControlChannel(peer,nextPeer,controlChannel,channel))return;
      gearRejectionInhibited=false;
      webrtcLabel.textContent='控制链路已连接';
      resetControlAuthorityInput();
      resetControlProfileSession();
      lastVehicleSafetyState='';
      gearChangeStationaryEvidence=controlLogic.createGearChangeStationaryEvidence();
      vcuHandshake={supported:false,state:'unavailable',ready:false,requested:false,disarming:false,parking_ready:false,driver_connected:true,adapter_ready:null};
      clientLog('control_datachannel_open');
      renderMonitoring();
    };
    channel.onmessage=async event=>{
      if(!controlLogic.isCurrentControlChannel(peer,nextPeer,controlChannel,channel))return;
      try{
        const message=JSON.parse(event.data);
        if(!['vehicle_telemetry','vcu_handshake_status','session_control_profile_status','control_command_rejected'].includes(message.event))return;
        if(!acceptControlStatusMessage(message))return;
        const gearRejectionState=message.event==='control_command_rejected'&&message.issue_code==='vcu_drive_gear_change_moving_or_stale'?controlLogic.reduceGearChangeRejection(pendingGearTransition,message,selectedGear):null;
        const gearRejectionMatched=Boolean(gearRejectionState?.matched);
        if(message.event==='control_command_rejected'){
          const rejection=controlLogic.deriveControlCommandRejection(message.issue_code);
          let rollbackFrom=null,rollbackTo=null;
          if(rejection.action==='rollback_gear_change'){
            if(gearRejectionMatched){rollbackFrom=pendingGearTransition.toGear;rollbackTo=pendingGearTransition.fromGear}
            selectedGear=gearRejectionState.selectedGear;pendingGearRequest=gearRejectionState.pendingGearRequest;pendingGearTransition=gearRejectionState.pendingGearTransition;gearRejectionInhibited=gearRejectionState.inhibitOrdinaryControl;clearControlInput(false);
            if(gearRejectionState.sendRollback)send({},false).catch(console.error);
          }else if(rejection.clearInput)clearControlInput();
          statusPanel.textContent=gearRejectionMatched?rejection.text:(rejection.action==='rollback_gear_change'?'换挡拒绝无法关联，普通控制已冻结；请安全断开并重新握手。':rejection.text);
          const commandSeq=Number(message.command_seq);
          clientLog('driver_control_command_rejected',{issue_code:rejection.issueCode,command_seq:Number.isSafeInteger(commandSeq)&&commandSeq>0?commandSeq:null,gear_rejection_matched:gearRejectionMatched,rollback_from:rollbackFrom,rollback_to:rollbackTo});
          renderMonitoring();
          return;
        }
        if(message.event==='session_control_profile_status'){
          applyControlProfileStatus(message);
          updateVehicleHardLimits(message.hard_limits);
          renderMonitoring();
          return;
        }
        if(message.event==='vehicle_telemetry'){
          vehicleTelemetry=message;
          if(controlLogic.telemetryConfirmsGearTransition(pendingGearTransition,message)){const completedTransition=pendingGearTransition;pendingGearTransition=null;pendingGearRequest=null;clientLog('driver_gear_transition_confirmed',{from_gear:completedTransition.fromGear,to_gear:completedTransition.toGear,control_status_seq:Number(message.control_status_seq)})}
          if(message.vcu_handshake){const nextVcuStatus={...message.vcu_handshake,driver_connected:true,adapter_ready:vcuAdapterReady(message.vcu_handshake,message.vehicle_adapter?.opened)};updateVcuHandshakeState(nextVcuStatus)}
          renderEstopRequest();
          applyControlProfileStatus(message.session_control_profile);
          updateVehicleHardLimits(message.control_limits);
          applyVehicleSafetyState(message.safety_state);
          renderMonitoring();
          return;
        }
        if(message.event!=='vcu_handshake_status')return;
        const nextVcuStatus=message.status||{};
        updateVcuHandshakeState({...nextVcuStatus,driver_connected:Boolean(message.driver_connected),adapter_ready:vcuAdapterReady(nextVcuStatus,message.adapter_ready)});
        updateVehicleHardLimits(message.hard_limits);
        if(message.session_control_profile)applyControlProfileStatus(message.session_control_profile);
        if(vcuHandshake.adapter_ready===false)statusPanel.textContent='VCU 适配器明确不可用；视频保持在线，驾驶命令已阻止';
        else if(vcuHandshake.adapter_ready===null)statusPanel.textContent='VCU 适配器状态未确认；视频保持在线，驾驶命令已阻止';
        else if(message.result==='command_rejected')statusPanel.textContent='开始握手失败：'+diagnoseVcuHandshake(true).text;
        else if(vcuHandshake.handshake_revoked)statusPanel.textContent=diagnoseVcuHandshake(true).text;
        else if(vcuDrivingReady())statusPanel.textContent='VCU 平行驾驶握手成功，可以发送驾驶命令';
        else if(vcuHandshake.state==='disarmed')statusPanel.textContent='VCU 握手已安全断开';
        clientLog('driver_vcu_handshake_status',{result:message.result,state:vcuHandshake.state,ready:Boolean(vcuHandshake.ready),parking_ready:Boolean(vcuHandshake.parking_ready),handshake_revoked:Boolean(vcuHandshake.handshake_revoked),vmc_fault_code:vcuHandshake.vmc_fault_code_valid?Number(vcuHandshake.vmc_fault_code):null});
        renderMonitoring();
      }catch(error){clientLog('control_datachannel_message_invalid',{error:error.message})}
    };
    channel.onclose=()=>{
      if(!controlLogic.isCurrentControlChannel(peer,nextPeer,controlChannel,channel))return;
      flushControlTrace('datachannel_close');
      controlChannel=null;
      resetControlAuthorityInput();
      resetControlProfileSession();
      vehicleTelemetry=null;
      lastVehicleSafetyState='';
      vcuHandshake={supported:false,state:'unavailable',ready:false,requested:false,disarming:false,parking_ready:false,driver_connected:false,adapter_ready:null};
      webrtcLabel.textContent='控制链路中断';
      clientLog('control_datachannel_closed');
      renderMonitoring();
    };
    channel.onerror=()=>{
      if(!controlLogic.isCurrentControlChannel(peer,nextPeer,controlChannel,channel))return;
      flushControlTrace('datachannel_error');
      clientLog('control_datachannel_error',{ready_state:channel.readyState,buffered_amount_bytes:Math.max(0,Number(channel.bufferedAmount)||0),peer_connection_state:nextPeer.connectionState});
      resetControlAuthorityInput();
      resetControlProfileSession();
      lastVehicleSafetyState='';
      webrtcLabel.textContent='控制链路错误';
      renderMonitoring();
    };
  };
  nextPeer.onicecandidate=e=>{if(e.candidate)post('/api/webrtc/ice-candidate',{candidate:e.candidate.toJSON()}).catch(console.error)};
  nextPeer.ontrack=e=>{const mid=e.transceiver.mid||'',id=offeredCameraByMid.get(mid)||remoteCameraIds.find(cameraId=>!assignedCameraIds.has(cameraId))||mid||e.track.id;assignedCameraIds.add(id);cameraByMid.set(mid,id);attach(id,e.track)};
  await nextPeer.setRemoteDescription({type:'offer',sdp:offer.sdp});
  while(pendingIce.length)await addIce(pendingIce.shift());
  const answer=await nextPeer.createAnswer();
  await nextPeer.setLocalDescription(answer);
  await post('/api/webrtc/answer',{type:'answer',sdp:nextPeer.localDescription.sdp});
}
async function collectMetrics(){
  if(!peer)return;
  const report=await peer.getStats(),sampledAt=Date.now();
  let rtt=0,connectionMethod='unknown',turnInUse=false,selectedPair=null;
  for(const s of report.values())if(s.type==='candidate-pair'&&s.state==='succeeded'&&(s.nominated||!selectedPair))selectedPair=s;
  if(selectedPair){rtt=Number(selectedPair.currentRoundTripTime||0);const local=report.get(selectedPair.localCandidateId),remote=report.get(selectedPair.remoteCandidateId),types=[local?.candidateType,remote?.candidateType];turnInUse=types.includes('relay');connectionMethod=turnInUse?'TURN':(types.some(type=>type==='srflx'||type==='prflx')?'STUN':'direct')}
  const streams=[];
  for(const s of report.values()){
    if(s.type!=='inbound-rtp'||(s.kind||s.mediaType)!=='video')continue;
    const statsKey=s.mid||String(s.ssrc||s.id),prior=previousStats.get(statsKey),decoded=Number(s.framesDecoded||0),bytesReceived=Number(s.bytesReceived||0),packetsLost=Number(s.packetsLost||0),packetsReceived=Number(s.packetsReceived||0);
    let fps=Number(s.framesPerSecond||0),bitrateKbps=0;
    if(prior){const seconds=(sampledAt-prior.sampledAt)/1000;if(seconds>0){if(!fps)fps=(decoded-prior.framesDecoded)/seconds;bitrateKbps=Math.max(0,(bytesReceived-prior.bytesReceived)*8/seconds/1000)}}
    previousStats.set(statsKey,{sampledAt,framesDecoded:decoded,bytesReceived});
    const jitterMs=Number(s.jitterBufferEmittedCount||0)>0?Number(s.jitterBufferDelay||0)*1000/Number(s.jitterBufferEmittedCount):0;
    const processingMs=decoded>0?Number(s.totalProcessingDelay||0)*1000/decoded:0;
    const cameraId=cameraByMid.get(s.mid||'')||'',lane=(mediaStatus.lanes||[]).find(l=>l.camera_id===cameraId)||{};
    const captureEncodeMs=Number(lane.capture_to_encoded_ms||0),latencyMs=captureEncodeMs+rtt*500+jitterMs+processingMs;
    streams.push({camera_id:cameraId,mid:s.mid||'',codec_id:s.codecId||'',fps,bitrate_kbps:bitrateKbps,frames_decoded:decoded,frames_dropped:Number(s.framesDropped||0),packets_lost:packetsLost,packets_received:packetsReceived,packet_loss_percent:(packetsLost+packetsReceived)>0?100*packetsLost/(packetsLost+packetsReceived):0,jitter_ms:Number(s.jitter||0)*1000,capture_to_encoded_ms:captureEncodeMs,jitter_buffer_ms:jitterMs,processing_ms:processingMs,round_trip_ms:rtt*1000,estimated_end_to_end_latency_ms:latencyMs,passed:fps>=20&&latencyMs<=200})
  }
  const timeSync=latestRuntimeStatus.time_sync||mediaStatus.time_sync||{},turnConfigured=hasTurnServer();
  const controlOutcomes={...controlOutcomeSession.metrics};
  const metrics={sampled_at_ms:sampledAt,connection_state:peer.connectionState,codec:mediaStatus.codec||'',backend:mediaStatus.backend||'',control_rtt_ms:rtt*1000,connection_method:connectionMethod,turn_configured:turnConfigured,turn_in_use:turnInUse,time_sync:timeSync,clock_uncertainty_ms:Number(timeSync.uncertainty_ms||0),latency_method:'capture-to-encoded + rtt/2 + jitter-buffer + browser-processing',control_outcomes:controlOutcomes,control_outcomes_balanced:controlLogic.controlOutcomesBalanced(controlOutcomes),streams,passed:streams.length>0&&streams.every(s=>s.passed)};
  await post('/api/webrtc/metrics',metrics);
  if(metrics.codec==='h265'&&metrics.connection_state==='connected'&&streams.length){h265FailureSamples=streams.some(s=>s.fps<20)?h265FailureSamples+1:0;if(h265FailureSamples>=3&&!h265FallbackSent){h265FallbackSent=true;await post('/api/webrtc/fallback',{codec:'h264',reason:'h265_decode_fps_below_20'})}}else h265FailureSamples=0;
  latestMetrics=metrics;renderMonitoring();statusPanel.textContent=`${metrics.connection_state||'等待连接'} · ${streams.length} 路视频 · RTT ${formatMetric(metrics.control_rtt_ms,1,' ms')} · ${metrics.connection_method||'unknown'}`
}
setInterval(()=>collectMetrics().catch(console.error),1000);
setInterval(()=>refreshRuntimeStatus().catch(console.error),1000);
setInterval(()=>flushControlTrace('interval'),1000);
setInterval(()=>refreshControlIntent().catch(console.error),intentRefreshIntervalMs);
setInterval(()=>{if(authenticated&&!connecting)refreshVehicles().catch(handleVehicleRefreshError)},5000);
}
void bootstrapConsole().catch(console.error);
