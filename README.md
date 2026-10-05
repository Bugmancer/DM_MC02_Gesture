# DM-MC02 Gesture

基于 DM-MC02 / STM32H723VG、BMI088 和 HAL 的板端现场学习动作识别原型。学习、模板匹配、拒绝判定和保存都在板上完成；增加动作无需重新编译或烧录。

上电自动识别，无需电脑或网页开启。学习示范由板载 KEY 按下开始、松开结束，达到设定次数后自动保存并恢复识别。可在板端设置参与识别的动作种类数 1～8、每种动作示范数 1～3、各动作的 RGB 颜色和灯色持续时间 0.1～30 秒（默认 3 秒）。识别使用运动过程中的滑动窗口 DTW，不要求先回到固定初始姿态或等待动作结束；新确认动作可立即换色。

## 编译与烧录

本次生成的固件位于：

- `MDK-ARM/Build/DM_MC02_Gesture.hex`
- `MDK-ARM/Build/DM_MC02_Gesture.bin`（起始地址 `0x08000000`）
- `MDK-ARM/Build/DM_MC02_Gesture.axf`（带调试信息）
- `MDK-ARM/Build/DM_MC02_Gesture.map`、`build.log`

命令行脚本与 uVision 构建统一输出到 `MDK-ARM/Build`。最新固件 `gesture-20261006-r9` 支持网页修改动作数量、示范次数、每槽 RGB 和灯色持续时间；修复候选匹配亮同一动作灯但只保持 200 ms 的问题，动作标识灯现在只表示确认识别。r9 需要更新固件一次；现有动作模板及 r8 配置兼容加载，旧配置的持续时间默认为 3 秒。新设置保存在外部 Flash，之后修改不需重新编译。电脑端空中画笔算法仍兼容 r5。

在项目根目录运行：

```powershell
powershell -ExecutionPolicy Bypass -File Tools/build.ps1 -Rebuild
```

脚本读取 Keil 工程的源码、宏和头文件目录，直接调用 Keil ARMCC 5、armasm、armlink 和 fromelf 做完整构建。其他机器可传 `-CompilerBin` 或设置 `ARMCC_BIN`。

也可用 Keil 打开 `MDK-ARM/DM_MC02_Gesture.uvprojx` 构建。重新用 CubeMX 生成后，先执行 `Tools/sync-project.ps1`，恢复自定义源码组、头文件目录和 `MDK-ARM/gesture.sct`。同步脚本保持逐元素换行及无 BOM 的 UTF-8 编码，以兼容 uVision 工程读取。应用入口、定时器回调、USB 接收/会话处理均在 USER CODE 区域。

LCD DMA 缓冲必须位于 AXI SRAM；自定义 scatter 已把 `.dma_buffer` 放到 `0x24000000`。ICache 开启，DCache 保持关闭，不要直接改回 CubeMX 默认 scatter。

## 第一次使用

推荐使用配套中文 GUI。双击项目根目录的 `Start-GestureGUI.cmd`，浏览器会打开本地控制台；首次启动需要 Python 3.10+ 和联网安装 pyserial、imufusion、numpy，后续运行不需要网络。停止服务使用 `Stop-GestureGUI.cmd`，仅关闭浏览器不会断开设备。

GUI 的 KEY 录制需要本次 `MDK-ARM/Build/DM_MC02_Gesture.hex` 固件。旧固件仍可连接和看曲线，但不支持 KEY 模式时会禁用学习入口；烧录新版一次后，之后增加动作无需重新烧录。

只接电源也能使用：左右选择空槽，OK 开始学习，按住 KEY 做动作、松开结束；重复到设定次数即自动保存并恢复识别。学习不要求先校准、静止等待或摆正板子；动作建议持续约 0.2～2 秒。设置入口是 UP，具体按键见下文。电脑 GUI 仅是可选的观察和快捷键工具；电脑快捷键仍需单独启用，不会随板端自动识别而自动开启。

“空间姿态”通过 USB 六轴数据展示固定世界坐标轴、相对方位、位置点和轨迹，可拖动视角和归零位置，不显示板子模型。“实时数据”查看六轴曲线；“采集记录”按用户、会话、标签和速度保存 CSV、事件和汇总。没有板子时可用“演示设备”走完整流程，演示数据有标记且不会发送键盘事件。详见 `GUI/README.md`。

“空中画笔”按住板载实体 KEY 记录三维笔迹，松开后回修整笔并固定位置，鼠标拖动换视角、滚轮缩放；未按 KEY 不会落笔。轨迹核心移植自 gaitmap 2.6.0 的 ESKF + RTS，使用静止零速观测、近似起止零速和逆向平滑；支持三轴开放笔画，不把终点拉回起点，也不约束 Z 为零。不显示板子或虚拟笔尖。支持换色、撤销、浏览器草稿、PNG 和 XYZ 米制 JSON 导出。

空间页与画笔使用同一套固定重力参考系和轨迹，Z 轴向上，归零只重置位置，不随板子倾斜参考轴。Fusion AHRS 连续估计姿态，为每笔提供初始方向。ESKF 内部仍有惯性预测，但由误差观测和整笔回修限制漂移；适合短笔画近似形状，不提供绝对定位。移植源码、MIT 许可和适配差异在 `GUI/trajectory.py`、`GUI/vendor/gaitmap-PORT.md`，仅依赖现有 numpy。

直接用板载按键也可以：

1. 上电即自动识别；尚无已存动作时等待学习，不要求先校准。
2. 左右选择空动作槽位，OK 开始学习，此时识别暂停。
3. 按住板载 KEY（PA15）开始一段示范，松开结束；达到设定的 1～3 段后自动保存、自动恢复识别。默认只需一段。
4. 相似示范仍计入并提示。无效示范可重试，已接受次数保留。DOWN 取消学习并恢复旧动作识别；写入失败保留示范，可按 OK 重试。
5. UP 打开板端设置。上下选择 `ACTION COUNT`、`DEMOS PER ACTION`、`RGB HOLD TIME`、`COLOR FOR ACTION`、`RED/GREEN/BLUE`、保存或取消；左右修改值，持续时间以 0.1 秒步进，RGB 行左右粗调 17、OK 细调 1，可设置完整 0～255。选 `SAVE AND EXIT` 后 OK 保存到 Flash 并恢复识别。
6. 动作种类数可设 1～8，示范数可设 1～3；每槽单独设置 RGB。缩小种类数只停用高编号槽位，扩大后原模板和颜色仍保留。
7. 已占用槽位按 OK 进入删除确认，再按 OK 删除并自动恢复识别；DOWN 取消。KEY 不用于开启或关闭识别。

网页“动作库”上方可选动作数量、示范次数和“灯色持续时间”，每槽 RGB 选择器修改灯色；点击“保存到板子”统一写入 Flash。网页等待板端确认，不将本地草稿当作保存成功。学习或删除确认期间不能修改配置；r8 可修改数量和颜色，持续时间控件需要 r9。

屏幕显示状态、槽位模板数、最佳/次佳距离、拒绝和漏采计数。以下是默认颜色，均可在板端逐槽修改，GUI 同步实际配置：

| 槽位 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 颜色 | 红 | 绿 | 蓝 | 黄 | 青 | 品红 | 橙 | 白 |

学习等待为暗蓝、按住录制亮当前槽位颜色、待重试保存为黄色、自动识别等待为青色。确认识别的灯色保持设置的时长，期间候选或陌生动作不会清除它，新确认动作立即换色并重新计时。候选仅更新匹配分数，不触发动作标识灯；进入学习/设置或硬件错误可以提前结束保持。陌生动作结束后双闪红色，硬件错误持续闪红。温控加热和对外电源输出保持关闭。

保存和校准会明确暂停采样，完成后使用新的有效 IMU 样本恢复。USB/LCD 不会阻塞等待电脑读取；意外漏采会计数并清除历史窗口，不会补造样本。确认识别更新 RGB；电脑操作事件单独防重复，持续运动不会连续发送同一串快捷键。

## 电脑事件与数据采集

USB 是 CDC 虚拟串口，使用 OTG_HS 控制器的内部 Full-Speed PHY。当前固件不枚举为 HID 键盘/手柄；随附电脑端桥接工具可以把动作事件映射为 Windows 快捷键。电脑只接收事件和采集数据，不参与训练或匹配。

安装依赖并采集：

```powershell
python -m pip install -r Tools/requirements.txt
python Tools/gesture_host.py --port COM7 --output captures/session01.csv --user u01 --session s01 --label circle --speed normal --duration 60
```

将 COM7 改为实际端口。工具默认请求 `stream 1` 和 `status`，板子独立自动识别。退出默认发送 `stream 0`，不关闭板端识别；串口断开后不自动重连。已有输出文件会被拒绝覆盖。

生成三个文件：

- CSV：用户、会话、标签、速度、主机时间、样本序号、板端时间，以及加速度 m/s2、角速度 rad/s。
- 同名 `.events.jsonl`：全部协议行及解析后的事件。
- 同名 `.summary.json`：样本数、序号缺口、已知/未知事件、最后状态和运行异常。

快捷键映射 JSON 示例：

```json
{"1": "ctrl+shift+k", "2": "alt+left", "3": "space"}
```

只有显式传入 `--hotkeys mapping.json` 才会发送 Windows 按键，默认为只采集。支持 Ctrl/Alt/Shift/Win、字母、数字、F1~F12、方向键和 Space；按键总会按反序释放。快捷键作用于当前前台程序。映射暂时保存在电脑 JSON 中，HID 和板上编辑映射留作后续扩展。

串口命令均以换行结尾：

| 命令 | 行为 |
| --- | --- |
| `status` | 输出状态和最大处理耗时 |
| `list` | 输出 INFO 和 8 条 SLOT，供 GUI 同步库存与学习状态 |
| `arm` / `disarm` | 启用/关闭识别，要求传感器就绪且有已保存类别，无强制校准 |
| `learn 1` | 在指定空槽位录制，槽位范围 1~8 |
| `save` | 确认保存已完成的示范，或确认待删除槽位 |
| `delete 1` | 进入删除确认，还需 `save` |
| `cancel` | 放弃待确认操作并恢复自动识别 |
| `calibrate` | 关闭识别后重新静止校准 |
| `stream 1` / `stream 0` | 开关原始六轴数据输出 |
| `configure N D RRGGBB ... H` | 设置动作数量 N、示范次数 D、完整 8 槽 RGB 和持续时间 H（毫秒）；H 可省略以保留当前时长。成功返回 `CONFIG_RESULT,SAVED`，失败返回 `CONFIG_RESULT,FAILED` |

协议 v1：

```text
RAW,seq,t_ms,ax_milli,ay_milli,az_milli,gx_milli,gy_milli,gz_milli,key_down
FIRMWARE,gesture-20261005-r5,KEY_CAPTURE,NO_CALIBRATION,RANDOM_START,LIVE_MATCH,ONE_DEMO,SIMILARITY_WARNING,RAW_KEY
TIMING,3000
TRAIN,WARN,SIMILAR,existing_slot_1based,distance_micro,limit_micro
MATCH,t_ms,class_id_or_0,distance_micro,second_distance_micro,duration_ms
EVENT,t_ms,class_id,distance_micro,second_distance_micro,duration_ms
UNKNOWN,t_ms,reason
STATUS,t_ms,armed,classes,calibrated,samples,sample_drops,imu_errors,unknown,usb_drops,max_feed_us,max_read_us
INFO,1,ready_mask,display_ok,training,selected_1based,progress,ready,delete_confirm
SLOT,slot_1based,template_count,name
CAPTURE,MODE,KEY
CAPTURE,WAIT|BEGIN|END,slot_1based
```

RAW 轴值除以 1000 转为 m/s2 和 rad/s，EVENT/MATCH 距离除以 1000000；次佳不存在时用 999999999。MATCH 类别 0 表示当前无可信匹配，1~8 对应槽位；MATCH 只更新可视反馈，不发送键盘事件。`TRAIN,READY,slot,count` 携带实际接受次数，旧版缺少 count 时按三次处理。USB 发送队列有界并为事件保留容量，慢主机可能丢原始数据，`usb_drops` 会计数。连接变更会清空旧事件和未完成命令，避免重连后执行历史动作。

## 算法与资源

`Algorithm/gesture_engine.*` 是无 HAL、无动态分配的独立 C99 库，接收带毫秒时间戳的六轴数据。板级适配、显示、USB 和状态机分别位于 Board、Display、App。

- 200 Hz 输入；学习仅在 KEY 按下到松开的边界内取样，无预录和首尾裁剪，最长 2.4 秒。超长、无效数据或漏采使本次示范无效，释放后拒绝，不截断保存。
- 板端启用连续识别：维护有界历史窗口，按模板时长的多个倍率重采样并使用已有 DTW 比较，每次采样最多计算一个类别的三个模板。完整窗口符合距离与类别间隔要求时输出 MATCH；动作事件再做邻近时间窗口确认，每段连续运动最多输出一次，静止 400 ms 后重新允许动作事件。RGB 匹配仍可在连续运动中切换类别。
- 独立库默认保留自动分段模式以兼容原调用者：预录 16 点，开始高阈值连续 6 点，结束安静 150 ms，最长 2.4 秒，结束后安静 400 ms 才重启。板端显式启用手动训练和连续识别。
- 归一化前要求至少 30 ms 且 6 点达到线性加速度 2.0 m/s2 或角速度 1.2 rad/s，避免同形轻晃被强度归一化放大。
- 重力跟踪后生成六维模长/重力轴有符号分量/水平模长特征；每次动作重采样为 48 点、int16。
- 每类保存 1~3 个真实模板，DTW 窗口半宽 8，使用两行 DP；有多段示范时类别距离取两条最近模板距离的平均，单段时使用该模板距离。
- 比较时两条序列各增加两个隐式静止端点，所有 48 个真实点仍参与，便于手动模板与自动分段对齐；工作区增加 16 B，DTW 单元格计算约增加 4.6%。已测试每端 50 ms 静止，长时间按住不动会占用模板和时长，不应据此宣称任意停顿都能匹配。
- 单段示范初始识别阈值为 0.10。可选补录允许示范两两距离达到 0.30，覆盖适度的速度、幅度差异；多段学习阈值由最坏距离推导，限制在 0.055~0.18。推导值超过识别上限时截到 0.18，不再因此拒绝示范。手动新增类与旧模板过于相似时，报告最近槽位、距离及相似边界作为提醒，仍接受示范；独立库自动训练保留冲突拒绝。识别仍需最佳/次佳差至少 0.025，且最佳不超过次佳的 0.78 倍，无法区分时拒绝，不强选颜色或快捷操作。
- 不自动把识别结果加入训练；示范只进入待确认区，按键保存前不改变已有模型。
- W25Q64 最后 128 KiB（`0x7e0000..0x7fffff`）专用于模型双槽，每槽 64 KiB。记录包含版本、长度、序号、CRC、最后提交标记；写新槽并校验后提交，保留旧槽用于掉电恢复。不要与其他固件数据混用该区域。

| 项目 | 当前成本 |
| --- | --- |
| 板端单个引擎对象 | 29324 B（当前 ARMCC map），含全部类别、采样缓存、544 点滚动历史和 DTW 工作区；主机编译器 ABI 可能不同 |
| 模型导出 | 固定 14128 B，显式小端格式及 CRC |
| 每类逻辑载荷 | 1764 B；8 槽静态预分配，新增类不再申请 RAM |
| LCD DMA 缓冲 | 2240 B，单次 4 行、约 0.60 ms SPI 线传输时间 |
| 整个固件 | 约 70.1 KiB ROM、90.5 KiB RW+ZI，含 16 KiB 栈；以构建 map 为准 |
| 实板延迟 | 尚待测量；STATUS 提供 DWT 测得的最大 ge_feed 和读取耗时 |

六轴数据无法提供绝对航向；水平镜像或只有航向区别的动作可能得到相似特征。允许保存并不意味着这些动作一定能可靠区分，网页会给出相似提醒。较轻、较慢的动作会受质量门影响。当前拒绝阈值是启发式参数，仍需真实负样本标定，不能保证日常环境零误触。

连续匹配每轮按类别数量分摊到约 20~120 ms 的采样过程，事件确认再做一轮完整分类。快速动作可能在动作完成后才出现首个可信结果，但匹配不依赖静止结束判定。每个采样最多执行三次 DTW；实板总循环还包含 RGB、ADC 和显示开销，不能仅凭 `max_feed_us` 推断总耗时低于 5 ms。

板级 BMI088 设置为加速度 400 Hz/OSR4/±12g、陀螺仪 400 Hz/47 Hz/±2000dps；前台 200 Hz 读取最新数据。SPI2 DMA 和 DRDY 引脚保留配置，本版读传感器使用短的有界轮询，DRDY 输出未启用。LCD 使用 SPI1 DMA；RGB 使用 SPI6；存储使用 OSPI2 的单线 SPI 指令，IO2/3 拉高作 /WP 和 /HOLD。五向按键为 PA5 16 位 ADC，40 ms 消抖，用户键 PA15 按下为低。

## 验证与后续实测

运行全部主机检查：

```powershell
powershell -ExecutionPolicy Bypass -File Tests/run_tests.ps1
```

脚本支持 `-GccPath` 和 `-PythonPath`。覆盖速度/强度/初始握持变化、增量学习、未知与冲突拒绝、序列化、Flash 逐字节断电与损坏回退、LCD DMA 状态、保存回滚、USB 重连及电脑端协议。

原自动分段模式的 8 小时测试是加速执行的合成时间流：200 Hz、5760000 点，包含时间戳回绕，已知动作正确识别 1439 次、未知拒绝 2400 次，所构造负样本误接受 0 次。连续识别的 20 分钟合成流产生 240 次正确事件、360 次陌生动作拒绝，构造负样本误接受 0 次；另覆盖不同初始握持方向、450/700/1150 ms 回放和满八类计算调度。八类调度夹具用于检查计算和历史缓存，不代表八个真实动作的数据集。它们都不是板子实跑或真实误触率测量，具体构造见 `Tests/test_gesture_engine.c`。

仍需实板完成：屏幕方向/颜色、按键阈值、IMU ID/轴向、Flash 断电保存、不同速度/握持/用户的识别率、静置/步行/拿放/陌生动作的误触次数每小时，以及 1/2/4/8 类的延迟与漏采。使用采集工具保存连续负样本，并按用户与会话分开训练和测试，避免相邻窗口泄漏。

接线来源为当前工作区 DM_MC02_BusScope、`D:\workspace\dm-mc02\例程` 下 IMU/LCD/WS2812/W25Q64/USB 例程，以及 `CtrBoard-H7_V1.0-240124.pdf` 原理图。HAL、CMSIS、USB 中间件和参考字模保留其原有许可约束。
