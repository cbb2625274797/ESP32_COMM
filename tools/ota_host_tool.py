"""
ESP32-C3 OTA 上位机工具
通过 WiFi 连接 ESP32，发送 .hex 固件文件并触发 OTA

依赖: Python 3.8+, tkinter (标准库), requests

用法:
    python ota_host_tool.py
"""
import os
import sys
import socket
import threading
import traceback
import struct
from datetime import datetime
from typing import Optional, List, Tuple

# 尝试导入 tkinter，失败时给出提示
try:
    import tkinter as tk
    from tkinter import filedialog, messagebox, ttk
except ImportError:
    print("错误: 缺少 tkinter 模块")
    print("Linux/Mac 请安装: pip install tkinter")
    print("Windows 通常自带 tkinter")
    sys.exit(1)

try:
    import requests
except ImportError:
    print("错误: 缺少 requests 模块")
    print("请安装: pip install requests")
    sys.exit(1)


# ===================== 配置 =====================
ESP32_IP = "192.168.4.1"       # ESP32 在 AP 模式下的 IP
ESP32_PORT = 80               # HTTP 端口
DEBUG_LOG_IP = "0.0.0.0"       # 监听所有接口
DEBUG_LOG_PORT = 6666          # UDP 日志监听端口
TARGET_CHIP = "GD32E508"       # 目标芯片（用于显示）
CHUNK_SIZE = 4096              # 每包大小（字节）
# =============================================


class LogHandler:
    """UDP 日志接收器"""

    def __init__(self, text_widget: 'tk.Text', port: int = DEBUG_LOG_PORT):
        self.text_widget = text_widget
        self.port = port
        self.running = False
        self.sock: Optional[socket.socket] = None
        self.thread: Optional[threading.Thread] = None

        # 颜色标签
        self.tag_configs = {
            '[E]': {'foreground': '#FF4444', 'background': '#FFE4E4'},
            '[W]': {'foreground': '#FF8800', 'background': '#FFF4E4'},
            '[I]': {'foreground': '#0088FF', 'background': '#E4F0FF'},
            '[D]': {'foreground': '#888888', 'background': '#F0F0F0'},
            '[C]': {'foreground': '#00CC66', 'background': '#E4FFE4'},
        }

    def start(self):
        """启动日志接收线程"""
        self.running = True
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind((DEBUG_LOG_IP, self.port))
        self.sock.settimeout(0.5)

        self.thread = threading.Thread(target=self._receive_loop, daemon=True)
        self.thread.start()

    def stop(self):
        """停止日志接收"""
        self.running = False
        if self.sock:
            self.sock.close()
        if self.thread:
            self.thread.join(timeout=1)

    def _receive_loop(self):
        """日志接收循环"""
        while self.running:
            try:
                data, addr = self.sock.recvfrom(1024)
                msg = data.decode('utf-8', errors='replace').strip()
                if msg:
                    self._append_log(msg)
            except socket.timeout:
                continue
            except Exception:
                pass

    def _append_log(self, msg: str):
        """在 Text 组件中追加日志"""
        # 根据日志级别应用颜色
        applied = False
        for prefix, config in self.tag_configs.items():
            if f'{prefix}' in msg or prefix in msg:
                self.text_widget.insert('end', msg + '\n', prefix)
                applied = True
                break

        if not applied:
            self.text_widget.insert('end', msg + '\n')

        # 自动滚动到底部
        self.text_widget.see('end')


class IntelHexParser:
    """Intel HEX 文件解析器

    支持标准 Intel HEX 格式（:BB AAAA TT DD... CC）
    """

    def __init__(self, filepath: str):
        self.filepath = filepath
        self.data: List[Optional[bytes]] = []  # 按地址索引的字节数据
        self.max_addr = 0
        self.min_addr = 0xFFFFFFFF

    def parse(self) -> Tuple[bool, str]:
        """解析 HEX 文件，返回 (成功标志, 消息)"""
        try:
            with open(self.filepath, 'r', encoding='utf-8') as f:
                for line_no, line in enumerate(f, 1):
                    line = line.strip()
                    if not line:
                        continue

                    ok, msg = self._parse_line(line, line_no)
                    if not ok:
                        return False, msg

            # 计算实际数据范围
            if self.min_addr == 0xFFFFFFFF:
                return False, "HEX 文件为空或不包含数据"

            return True, f"解析成功，地址范围: 0x{self.min_addr:06X} - 0x{self.max_addr:06X}，共 {self.max_addr - self.min_addr + 1} 字节"

        except FileNotFoundError:
            return False, f"文件不存在: {self.filepath}"
        except Exception as e:
            return False, f"解析失败: {e}"

    def _parse_line(self, line: str, line_no: int) -> Tuple[bool, str]:
        """解析单行 HEX"""
        if not line.startswith(':'):
            return False, f"第 {line_no} 行格式错误，缺少 ':' 前缀"

        if len(line) < 11:
            return False, f"第 {line_no} 行长度不足"

        try:
            # 解析头部
            byte_count = int(line[1:3], 16)
            address = int(line[3:7], 16)
            record_type = int(line[7:9], 16)

            # 验证数据长度
            expected_len = 9 + byte_count * 2 + 2
            if len(line) != expected_len:
                return False, f"第 {line_no} 行长度不匹配，期望 {expected_len} 实际 {len(line)}"

            # 解析数据
            data_bytes = bytes.fromhex(line[9:9 + byte_count * 2])

            # 验证 CRC
            checksum = int(line[-2:], 16)
            calc_checksum = self._calc_checksum(line[1:-2])
            if calc_checksum != checksum:
                return False, f"第 {line_no} 行 CRC 校验失败"

        except ValueError as e:
            return False, f"第 {line_no} 行数据解析错误: {e}"

        # 处理不同类型的记录
        if record_type == 0x00:
            # 数据记录
            abs_addr = address
            if self.min_addr == 0xFFFFFFFF or abs_addr < self.min_addr:
                self.min_addr = abs_addr
            if abs_addr + byte_count - 1 > self.max_addr:
                self.max_addr = abs_addr + byte_count - 1

            # 存储数据（简化处理：直接存储）
            if len(self.data) < abs_addr + byte_count:
                self.data.extend([None] * (abs_addr + byte_count - len(self.data)))
            for i, b in enumerate(data_bytes):
                self.data[abs_addr + i] = b

        elif record_type == 0x01:
            # 文件结束记录
            pass

        elif record_type == 0x02:
            # 扩展段地址
            pass

        elif record_type == 0x04:
            # 扩展线性地址
            pass

        return True, ""

    def _calc_checksum(self, hex_str: str) -> int:
        """计算校验和"""
        return sum(int(hex_str[i:i+2], 16) for i in range(0, len(hex_str), 2)) & 0xFF

    def get_data_segment(self, start: int, length: int) -> bytes:
        """获取指定范围的数据，不足部分填充 0xFF"""
        result = bytearray()
        for i in range(length):
            addr = start + i
            if addr < len(self.data) and self.data[addr] is not None:
                result.append(self.data[addr])
            else:
                result.append(0xFF)
        return bytes(result)

    def get_total_size(self) -> int:
        """获取总数据大小"""
        if self.min_addr == 0xFFFFFFFF:
            return 0
        return self.max_addr - self.min_addr + 1


class OTAClient:
    """OTA 客户端，与 ESP32 通信"""

    def __init__(self, esp_ip: str = ESP32_IP, esp_port: int = ESP32_PORT):
        self.esp_ip = esp_ip
        self.esp_port = esp_port
        self.base_url = f"http://{esp_ip}:{esp_port}"

    def upload_firmware(self, filepath: str,
                        progress_callback=None,
                        chunk_size: int = CHUNK_SIZE) -> Tuple[bool, str]:
        """上传固件到 ESP32

        Args:
            filepath: HEX 文件路径
            progress_callback: 进度回调函数，参数为 (当前字节, 总字节)

        Returns:
            (成功标志, 消息)
        """
        # 解析 HEX 文件
        parser = IntelHexParser(filepath)
        ok, msg = parser.parse()
        if not ok:
            return False, msg

        total_size = parser.get_total_size()
        print(f"[OTA] 固件大小: {total_size} 字节")

        # 分块发送
        addr = parser.min_addr
        sent_bytes = 0
        seq = 0

        while addr <= parser.max_addr:
            # 计算本块大小
            remaining = parser.max_addr - addr + 1
            chunk = min(chunk_size, remaining)

            # 读取数据
            data = parser.get_data_segment(addr, chunk)

            # 构造数据包: SEQ(2B) + SIZE(4B) + DATA
            packet = struct.pack('<H', seq) + struct.pack('<I', total_size) + data

            # 发送
            try:
                response = requests.post(
                    f"{self.base_url}/ota/upload",
                    data=packet,
                    timeout=10,
                    headers={'Content-Type': 'application/octet-stream'}
                )

                if response.status_code != 200:
                    return False, f"上传失败，HTTP {response.status_code}: {response.text}"

            except requests.exceptions.RequestException as e:
                return False, f"网络错误: {e}"

            sent_bytes += chunk
            addr += chunk
            seq += 1

            if progress_callback:
                progress_callback(sent_bytes, total_size)

        # 检查存储完成状态
        status_ok, status_msg = self.check_status()
        if not status_ok:
            return False, f"固件已发送但状态检查失败: {status_msg}"

        return True, f"固件上传完成，共 {seq} 包，{sent_bytes} 字节"

    def trigger_relay(self) -> Tuple[bool, str]:
        """触发 ESP32 开始 OTA 中继"""
        try:
            response = requests.post(f"{self.base_url}/ota/relay", timeout=5)
            if response.status_code == 200:
                return True, "OTA 中继已触发"
            return False, f"触发失败: HTTP {response.status_code}"
        except Exception as e:
            return False, f"网络错误: {e}"

    def check_status(self) -> Tuple[bool, str]:
        """查询 OTA 状态"""
        try:
            response = requests.get(f"{self.base_url}/ota/status", timeout=5)
            if response.status_code == 200:
                return True, response.text
            return False, f"查询失败: HTTP {response.status_code}"
        except Exception as e:
            return False, f"网络错误: {e}"

    def health_check(self) -> Tuple[bool, str]:
        """查询 ESP32 健康状态"""
        try:
            response = requests.get(f"{self.base_url}/health", timeout=5)
            if response.status_code == 200:
                return True, response.text
            return False, f"查询失败: HTTP {response.status_code}"
        except Exception as e:
            return False, f"网络错误: {e}"


class OTAToolGUI:
    """OTA 上位机主界面"""

    def __init__(self):
        self.root = tk.Tk()
        self.root.title(f"ESP32 OTA 上位机工具 - {TARGET_CHIP}")
        self.root.geometry("900x700")

        # ESP32 连接配置
        self.esp_ip = tk.StringVar(value=ESP32_IP)

        # OTA 客户端
        self.ota_client = OTAClient()

        # 日志接收器
        self.log_handler: Optional[LogHandler] = None

        # 当前的 HEX 文件
        self.current_hex_file = None

        self._create_widgets()
        self._start_log_receiver()

    def _create_widgets(self):
        """创建界面组件"""
        # ----- 顶部：连接配置 -----
        frame_conn = tk.LabelFrame(self.root, text="ESP32 连接配置", padx=10, pady=5)
        frame_conn.pack(fill='x', padx=10, pady=5)

        tk.Label(frame_conn, text="ESP32 IP:").grid(row=0, column=0, sticky='e')
        tk.Entry(frame_conn, textvariable=self.esp_ip, width=20).grid(row=0, column=1, padx=5)

        tk.Button(frame_conn, text="连接测试", command=self._test_connection).grid(row=0, column=2, padx=5)
        tk.Button(frame_conn, text="健康检查", command=self._health_check).grid(row=0, column=3, padx=5)

        # ----- 中部左侧：文件选择 -----
        frame_file = tk.LabelFrame(self.root, text="固件文件", padx=10, pady=5)
        frame_file.pack(fill='both', expand=True, side='left', padx=10, pady=5)

        self.file_path_var = tk.StringVar(value="未选择文件")
        tk.Label(frame_file, textvariable=self.file_path_var, fg='blue', wraplength=300).pack(fill='x')

        tk.Button(frame_file, text="选择 .hex 文件", command=self._select_hex_file).pack(pady=5)
        tk.Button(frame_file, text="选择 .bin 文件", command=self._select_bin_file).pack(pady=5)

        # 文件信息
        self.file_info_var = tk.StringVar(value="")
        tk.Label(frame_file, textvariable=self.file_info_var, fg='gray').pack(fill='x')

        # 上传按钮
        self.upload_btn = tk.Button(frame_file, text="开始上传固件", bg='green', fg='white',
                                    font=('Arial', 12, 'bold'), state='disabled',
                                    command=self._start_upload)
        self.upload_btn.pack(fill='x', pady=10)

        # 进度条
        self.progress_var = tk.DoubleVar(value=0)
        self.progress_bar = ttk.Progressbar(frame_file, variable=self.progress_var, maximum=100)
        self.progress_bar.pack(fill='x', pady=5)

        self.progress_label = tk.Label(frame_file, text="", fg='gray')
        self.progress_label.pack()

        # 触发 OTA 中继
        tk.Button(frame_file, text="触发 ESP32 OTA 中继", bg='orange',
                  command=self._trigger_relay).pack(fill='x', pady=5)

        # ----- 中部右侧：调试日志 -----
        frame_log = tk.LabelFrame(self.root, text="调试日志 (UDP:6666)", padx=5, pady=5)
        frame_log.pack(fill='both', expand=True, side='right', padx=10, pady=5)

        # 日志文本框
        self.log_text = tk.Text(frame_log, height=25, width=70, font=('Courier New', 9))
        self.log_text.pack(fill='both', expand=True)

        # 配置日志颜色标签
        self.log_text.tag_config('[E]', foreground='#FF4444')
        self.log_text.tag_config('[W]', foreground='#FF8800')
        self.log_text.tag_config('[I]', foreground='#0088FF')
        self.log_text.tag_config('[D]', foreground='#888888')

        # 滚动条
        scrollbar = tk.Scrollbar(self.log_text, command=self.log_text.yview)
        scrollbar.pack(side='right', fill='y')
        self.log_text.config(yscrollcommand=scrollbar.set)

        # 清除日志按钮
        tk.Button(frame_log, text="清除日志", command=lambda: self.log_text.delete('1.0', 'end')).pack()

    def _start_log_receiver(self):
        """启动 UDP 日志接收"""
        self.log_handler = LogHandler(self.log_text, DEBUG_LOG_PORT)
        self.log_handler.start()
        self._log_info("上位机启动", "UDP 日志接收已启动，监听 0.0.0.0:6666")

    def _test_connection(self):
        """测试与 ESP32 的连接"""
        ip = self.esp_ip.get().strip()
        if not ip:
            messagebox.showerror("错误", "请输入 ESP32 的 IP 地址")
            return

        self.ota_client = OTAClient(esp_ip=ip)
        ok, msg = self.ota_client.health_check()
        if ok:
            messagebox.showinfo("连接成功", f"ESP32 连接正常\n{msg}")
            self._log_info("连接", f"ESP32 ({ip}) 连接成功")
        else:
            messagebox.showerror("连接失败", f"无法连接到 ESP32\n{msg}")
            self._log_error("连接", f"ESP32 ({ip}) 连接失败: {msg}")

    def _health_check(self):
        """查询 ESP32 健康状态"""
        ip = self.esp_ip.get().strip()
        self.ota_client = OTAClient(esp_ip=ip)
        ok, msg = self.ota_client.health_check()
        if ok:
            self._log_info("健康检查", msg)
            messagebox.showinfo("健康状态", msg)
        else:
            self._log_error("健康检查", f"查询失败: {msg}")
            messagebox.showerror("健康检查", f"查询失败\n{msg}")

    def _select_hex_file(self):
        """选择 HEX 文件"""
        filepath = filedialog.askopenfilename(
            title="选择 HEX 文件",
            filetypes=[("HEX 文件", "*.hex"), ("所有文件", "*.*")]
        )
        if filepath:
            self.current_hex_file = (filepath, 'hex')
            self.file_path_var.set(os.path.basename(filepath))
            self._log_info("文件", f"已选择: {filepath}")

            # 解析文件信息
            parser = IntelHexParser(filepath)
            ok, msg = parser.parse()
            if ok:
                self.file_info_var.set(msg)
                self.upload_btn.config(state='normal')
            else:
                self.file_info_var.set(f"解析失败: {msg}")
                self.upload_btn.config(state='disabled')

    def _select_bin_file(self):
        """选择 BIN 文件（简化支持）"""
        filepath = filedialog.askopenfilename(
            title="选择 BIN 文件",
            filetypes=[("BIN 文件", "*.bin"), ("所有文件", "*.*")]
        )
        if filepath:
            self.current_hex_file = (filepath, 'bin')
            self.file_path_var.set(os.path.basename(filepath))
            self._log_info("文件", f"已选择: {filepath}")

            # 获取文件大小
            size = os.path.getsize(filepath)
            self.file_info_var.set(f"BIN 文件，大小: {size} 字节 ({size/1024:.1f} KB)")
            self.upload_btn.config(state='normal')

    def _start_upload(self):
        """开始上传固件"""
        if not self.current_hex_file:
            messagebox.showerror("错误", "请先选择固件文件")
            return

        filepath, file_type = self.current_hex_file

        self.upload_btn.config(state='disabled', text="上传中...")
        self.progress_var.set(0)
        self.progress_label.config(text="正在连接 ESP32...")

        # 在后台线程执行上传
        threading.Thread(target=self._upload_worker, args=(filepath, file_type), daemon=True).start()

    def _upload_worker(self, filepath: str, file_type: str):
        """上传工作线程"""
        try:
            if file_type == 'hex':
                ok, msg = self._upload_hex(filepath)
            else:
                ok, msg = self._upload_bin(filepath)

            if ok:
                self.root.after(0, lambda: self.progress_label.config(text="上传完成"))
                self.root.after(0, lambda: messagebox.showinfo("完成", msg))
                self._log_info("上传", msg)
            else:
                self.root.after(0, lambda: self.progress_label.config(text="上传失败"))
                self.root.after(0, lambda: messagebox.showerror("上传失败", msg))
                self._log_error("上传", f"失败: {msg}")

        except Exception as e:
            self.root.after(0, lambda: self.progress_label.config(text="上传异常"))
            self.root.after(0, lambda: messagebox.showerror("异常", f"上传异常: {e}\n{traceback.format_exc()}"))
            self._log_error("上传", f"异常: {e}")

        finally:
            self.root.after(0, lambda: self.upload_btn.config(state='normal', text="开始上传固件"))

    def _upload_hex(self, filepath: str) -> Tuple[bool, str]:
        """上传 HEX 文件"""
        parser = IntelHexParser(filepath)
        ok, msg = parser.parse()
        if not ok:
            return False, msg

        total_size = parser.get_total_size()
        addr = parser.min_addr
        seq = 0

        while addr <= parser.max_addr:
            remaining = parser.max_addr - addr + 1
            chunk = min(CHUNK_SIZE, remaining)
            data = parser.get_data_segment(addr, chunk)

            packet = struct.pack('<H', seq) + struct.pack('<I', total_size) + data

            response = requests.post(
                f"http://{self.esp_ip.get()}:{ESP32_PORT}/ota/upload",
                data=packet,
                timeout=10,
                headers={'Content-Type': 'application/octet-stream'}
            )

            if response.status_code != 200:
                return False, f"HTTP {response.status_code}: {response.text}"

            sent = min(addr - parser.min_addr + chunk, total_size)
            progress = (sent / total_size) * 100
            self.root.after(0, lambda p=progress, s=sent, t=total_size:
                          [self.progress_var.set(p),
                           self.progress_label.config(text=f"上传中: {s}/{t} 字节 ({p:.1f}%)")])

            addr += chunk
            seq += 1

        return True, f"HEX 文件上传完成，共 {seq} 包"

    def _upload_bin(self, filepath: str) -> Tuple[bool, str]:
        """上传 BIN 文件（直接二进制）"""
        file_size = os.path.getsize(filepath)

        with open(filepath, 'rb') as f:
            seq = 0
            offset = 0
            while offset < file_size:
                chunk = f.read(CHUNK_SIZE)
                packet = struct.pack('<H', seq) + struct.pack('<I', file_size) + chunk

                response = requests.post(
                    f"http://{self.esp_ip.get()}:{ESP32_PORT}/ota/upload",
                    data=packet,
                    timeout=10,
                    headers={'Content-Type': 'application/octet-stream'}
                )

                if response.status_code != 200:
                    return False, f"HTTP {response.status_code}: {response.text}"

                offset += len(chunk)
                seq += 1

                progress = (offset / file_size) * 100
                self.root.after(0, lambda p=progress, s=offset, t=file_size:
                              [self.progress_var.set(p),
                               self.progress_label.config(text=f"上传中: {s}/{t} 字节 ({p:.1f}%)")])

        return True, f"BIN 文件上传完成，共 {seq} 包"

    def _trigger_relay(self):
        """触发 OTA 中继"""
        ip = self.esp_ip.get().strip()
        self.ota_client = OTAClient(esp_ip=ip)
        ok, msg = self.ota_client.trigger_relay()
        if ok:
            self._log_info("触发", msg)
            messagebox.showinfo("成功", msg)
        else:
            self._log_error("触发", f"失败: {msg}")
            messagebox.showerror("失败", msg)

    def _log_info(self, tag: str, msg: str):
        """记录信息日志"""
        self._log(tag, msg, '[I]')

    def _log_error(self, tag: str, msg: str):
        """记录错误日志"""
        self._log(tag, msg, '[E]')

    def _log(self, tag: str, msg: str, level: str):
        """追加日志到文本框"""
        timestamp = datetime.now().strftime("%H:%M:%S")
        log_line = f"[{timestamp}]{level}[{tag}] {msg}"
        self.log_handler._append_log(log_line)

    def run(self):
        """启动界面主循环"""
        self.root.mainloop()

    def cleanup(self):
        """清理资源"""
        if self.log_handler:
            self.log_handler.stop()


def main():
    """主函数"""
    print(f"="*50)
    print(f"ESP32 OTA 上位机工具")
    print(f"ESP32 IP: {ESP32_IP}:{ESP32_PORT}")
    print(f"日志监听: {DEBUG_LOG_IP}:{DEBUG_LOG_PORT}")
    print(f"="*50)

    app = OTAToolGUI()
    try:
        app.run()
    finally:
        app.cleanup()


if __name__ == '__main__':
    main()