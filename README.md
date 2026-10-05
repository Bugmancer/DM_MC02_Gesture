# DM-MC02 Gesture

基于 DM-MC02 / STM32H723VG、BMI088 和 HAL 的板端现场学习动作识别原型。学习、模板匹配、拒绝判定和保存都在板上完成；增加动作无需重新编译或烧录。

学习示范由板载 KEY 按下开始、松开结束，一段有效示范即可保存，也可在保存前补录至三段。识别使用运动过程中的滑动窗口 DTW，持续更新匹配结果和 RGB，不要求先回到固定初始姿态或等待动作结束。此前已验证实板 USB 数据与校准，新版连续匹配的识别率和真实误触率仍待实测。

## 编译与烧录

本次生成的固件位于：

- `MDK-ARM/Build/DM_MC02_Gesture.hex`
- `MDK-ARM/Build/DM_MC02_Gesture.bin`（起始地址 `0x08000000`）
- `MDK-ARM/Build/DM_MC02_Gesture.axf`（带调试信息）
- `MDK-ARM/Build/DM_MC02_Gesture.map`、`build.log`

命令行脚本与 uVision 构建统一输出到 `MDK-ARM/Build`。旧目录 `MDK-ARM/DM_MC02_Gesture` 中可能仍有历史文件，请使用上面的路径。连接后 GUI 应显示 `gesture-20261005-r5`。r5 增加三维空中画笔所需的实体 KEY 采样；r4 起保留有效的相似示范并提示相似槽位。更旧固件还可能有校准或起始静止限制。

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

在 GUI 中选择 STM32 串口 → 连接 → 选空槽位准备学习 → 按住 KEY 做动作、松开结束 → 保存动作 → 开启动作识别。一段有效示范就能保存，额外两段是可选补录。学习不要求先校准、静止等待或摆正板子；动作建议持续约 0.2~2 秒，动作完成即松开 KEY。静止校准保留为可选的陀螺仪零偏修正。识别时无需按 KEY，匹配颜色在运动中更新。电脑快捷键可填 `ctrl+shift+k`、`alt+left` 或 `space`，修改后移开输入框即保存；还需单独打开“电脑快捷键”开关才会发送按键。顶部“停止”随时取消当前操作并关闭输出。

“空间姿态”通过 USB 六轴数据展示可旋转的三维板子、相对方位、位移估计和轨迹，可随时归零。“实时数据”查看六轴曲线；“采集记录”按用户、会话、标签和速度保存 CSV、事件和汇总。没有板子时可用“演示设备”走完整流程，演示数据有标记且不会发送键盘事件。详见 `GUI/README.md`。

“空中画笔”按住板载实体 KEY 记录固定世界坐标中的三维笔迹、松开固定位置，鼠标拖动换视角、滚轮缩放；未按 KEY 不会落笔。不显示板子，不把板子旋转当作笔尖位移。位置只在按键期间积分，配合静止速度清零、死区和速度衰减抑制漂移。支持换色、撤销、浏览器草稿、视图 PNG 和 XYZ 米制 JSON 导出。需要 r5 固件的 `RAW_KEY` 能力；六轴惯性轨迹仍是短时相对估计，没有绝对航向或绝对定位能力。

空间视图使用 Fusion AHRS，方向与位移统一相对于连接或归零时的板子坐标。无磁力计不能获得绝对航向；位移来自惯性积分，会漂移，只适合短时相对运动观察。静止清速度，断流不积分，超过 20 m 或 10 m/s 时提示归零并冻结位移，姿态仍显示。

直接用板载按键也可以：

1. 烧录后上电即开始采样，不再执行强制静止校准。
2. 左右选择空动作槽位，按 OK 开始学习。共有 8 个槽位。
3. 按住板载 KEY（PA15 的用户键）开始录制，做一次约 0.2~2 秒的明显动作后松开结束。无需先静止；按住期间停顿不会自动结束，也不会裁掉首尾静止段。一段接受后即可保存，最多补录三段。
4. 与已有动作相似时，示范仍会计入并可保存，屏幕提示相似的槽位。补录与本次已有示范差异过大、幅度太小或动作过长仍会拒绝，已接受次数保留。按 DOWN 可取消整次学习。
5. READY 后按 OK，写入外部 Flash。看到 `Saved` 后，按 UP 启用识别。
6. 默认上电不启用动作输出。UP 切换识别；KEY 只控制学习录制，不切换识别开关。DOWN 取消当前操作并关闭识别。
7. 已占用槽位按 OK 进入删除确认，再按 OK 删除并保存；DOWN 取消。

屏幕显示状态、槽位模板数、最佳/次佳距离、拒绝和漏采计数。识别成功时 RGB 按槽位显示固定颜色，GUI 同时显示对应色标：

| 槽位 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 颜色 | 红 | 绿 | 蓝 | 黄 | 青 | 品红 | 橙 | 白 |

学习等待为暗蓝、按住录制亮当前槽位颜色、待保存为黄色、识别启用为青色。可信匹配立即亮对应槽位颜色并保持至少 200 ms，确认的动作事件保持 1.2 秒，新匹配到另一类时立即换色。陌生动作结束后双闪红色，错误持续闪红，与槽位 1 的成功常亮区分。温控加热和对外电源输出保持关闭。

保存和校准会明确暂停采样，完成后使用新的有效 IMU 样本恢复。USB/LCD 不会阻塞等待电脑读取；意外漏采会计数并清除历史窗口，不会补造样本。实时匹配更新 RGB；电脑操作事件单独防重复，持续运动不会连续发送同一串快捷键。

## 电脑事件与数据采集

USB 是 CDC 虚拟串口，使用 OTG_HS 控制器的内部 Full-Speed PHY。当前固件不枚举为 HID 键盘/手柄；随附电脑端桥接工具可以把动作事件映射为 Windows 快捷键。电脑只接收事件和采集数据，不参与训练或匹配。

安装依赖并采集：

```powershell
python -m pip install -r Tools/requirements.txt
python Tools/gesture_host.py --port COM7 --output captures/session01.csv --user u01 --session s01 --label circle --speed normal --duration 60
```

将 COM7 改为实际端口。工具默认请求 `stream 1` 和 `status`，不自动启用识别。需要时加 `--command arm`，或在板上按 UP。退出默认发送 `stream 0` 和 `disarm`，串口断开后不自动重连。已有输出文件会被拒绝覆盖。

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
| `cancel` | 放弃待确认操作并关闭识别 |
| `calibrate` | 关闭识别后重新静止校准 |
| `stream 1` / `stream 0` | 开关原始六轴数据输出 |

协议 v1：

```text
RAW,seq,t_ms,ax_milli,ay_milli,az_milli,gx_milli,gy_milli,gz_milli,key_down
FIRMWARE,gesture-20261005-r5,KEY_CAPTURE,NO_CALIBRATION,RANDOM_START,LIVE_MATCH,ONE_DEMO,SIMILARITY_WARNING,RAW_KEY
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
