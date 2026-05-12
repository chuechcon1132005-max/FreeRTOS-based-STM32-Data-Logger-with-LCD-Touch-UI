# [HƯỚNG DẪN CHI TIẾT] Cấu hình và Lập trình Cảm ứng XPT2046 trên STM32

Tài liệu này tập trung chuyên sâu vào việc triển khai chức năng cảm ứng điện trở sử dụng IC **XPT2046**. Nội dung bao gồm phân tích sự khác biệt so với driver gốc của Waveshare (Open405R) và các kỹ thuật tối ưu hóa tín hiệu thực tế.

---

## 1. Phân tích sự khác biệt với Example chính chủ (Open405R)

Dựa trên việc phân tích thư viện `LCD28` từ Waveshare, project hiện tại đã có những điều chỉnh quan trọng để tối ưu và tương thích với môi trường hiện đại:

| Đặc điểm | Example Open405R (Gốc) | Project Hiện tại (Đã điều chỉnh) |
| :--- | :--- | :--- |
| **Thư viện** | Standard Peripheral Library (SPL) | **STM32 HAL Library** (Hỗ trợ tốt hơn cho CubeMX/IDE) |
| **Quản lý Bus** | SPI đơn nhiệm cho LCD | **Chia sẻ bus SPI1** giữa LCD và Touch thông qua quản lý CS riêng biệt. |
| **Tốc độ SPI** | Cố định cho LCD | **Chuyển đổi Baudrate linh hoạt**: Tự động hạ tốc độ SPI xuống `Prescaler 64` khi đọc Touch để đảm bảo độ chính xác của ADC XPT2046. |
| **Xử lý nhiễu** | Cơ bản hoặc không có | **Lọc trung vị (Median) + Kiểm tra độ ổn định (Stability Span)**. |
| **Tích hợp** | Driver rời rạc | Tích hợp sâu vào **FreeRTOS Task**, xử lý sự kiện touch theo thời gian thực. |

---

## 2. Cấu hình phần cứng chuyên sâu

### Kết nối chân tín hiệu (Pinout)
Theo đúng sơ đồ của Open405R nhưng được quản lý qua HAL:
- **TP_CS (PB9):** Chân chọn chip cảm ứng (Active Low).
- **TP_IRQ (PB4):** Chân ngắt báo có chạm (Active Low). Cần cấu hình `Input Pull-up`.
- **SPI1 (Shared):** SCK (PA5), MISO (PA6), MOSI (PA7).

### Quản lý tốc độ SPI
Đây là phần quan trọng nhất để cảm ứng không bị "nhảy" hoặc sai lệch. IC cảm ứng XPT2046 không thể chạy ở tốc độ cao như màn hình ST7789.
- **LCD SPI Speed:** Thường chạy ở Prescaler 2 hoặc 4 (tốc độ cao).
- **Touch SPI Speed:** Phải hạ xuống Prescaler 64 hoặc cao hơn.

---

## 3. Quy trình đọc dữ liệu Low-level

Để lấy tọa độ chính xác, quy trình giao tiếp SPI diễn ra như sau:

1. **Hạ tốc độ SPI:** Lưu cấu hình hiện tại và chuyển sang tốc độ thấp.
2. **Kích hoạt TCS:** Kéo chân `TP_CS` xuống LOW.
3. **Gửi mã lệnh (Control Byte):**
   - Đọc X: `0xD0` (12-bit mode, Differential, Power down between conversions).
   - Đọc Y: `0x90`.
4. **Nhận kết quả:** Nhận 2 byte từ SPI, ghép lại thành giá trị 12-bit (0 - 4095).
5. **Hủy kích hoạt TCS:** Kéo chân `TP_CS` lên HIGH.
6. **Khôi phục tốc độ SPI:** Trả lại tốc độ cao cho LCD.

---

## 4. Thuật toán xử lý tín hiệu (Signal Processing)

Để cảm ứng mượt mà và không bị điểm giả (ghost touches), project áp dụng hai lớp bảo vệ:

### Lớp 1: Lọc trung vị (Median Filter 3-point)
Đọc 3 lần liên tiếp cho mỗi trục, lấy giá trị ở giữa để loại bỏ nhiễu spike đột ngột.
```c
static uint16_t median3(uint16_t a, uint16_t b, uint16_t c) {
    // Sắp xếp và trả về giá trị trung vị
}
```

### Lớp 2: Kiểm tra độ ổn định (Stable Span Check)
Chỉ chấp nhận tọa độ nếu khoảng cách giữa giá trị lớn nhất và nhỏ nhất trong 3 lần đọc không vượt quá một ngưỡng (`TOUCH_STABLE_SPAN_MAX`, ví dụ 180 đơn vị ADC). Nếu vượt quá, coi như dữ liệu bị nhiễu và bỏ qua lần chạm đó.

---

## 5. Calibration và Hệ tọa độ Screen

Giá trị ADC 12-bit cần được ánh xạ sang pixel màn hình (240x320).

### Cấu hình ánh xạ (Mapping)
Cần xác định các hằng số thông qua thực tế (Calibration):
- `TOUCH_RAW_X_MIN/MAX`: Giới hạn ADC trục X.
- `TOUCH_RAW_Y_MIN/MAX`: Giới hạn ADC trục Y.

### Xử lý hướng (Orientation)
- **Swap XY:** Nếu xoay màn hình ngang/dọc.
- **Invert X/Y:** Nếu hướng cảm ứng ngược với hướng vẽ của LCD.

---

## 6. Xử lý lỗi vật lý (Dead Zones)

Một thực tế thường gặp là tấm nền cảm ứng bị hỏng hoặc liệt ở một số vùng (ví dụ góc phải phía dưới).
- **Giải pháp phần mềm:** Thay vì cố gắng sửa lỗi phần cứng, hãy thiết kế UI tránh vùng đó.
- **Hit Detection:** Sử dụng các hàm kiểm tra vùng (`ui_in_rect`) và thiết kế nút bấm có vùng nhận diện (Margin) rộng hơn hình ảnh hiển thị để tăng trải nghiệm người dùng.
- **Safe Zone:** Di chuyển các nút điều hướng quan trọng sang "vùng an toàn" (thường là nửa trái màn hình nếu nửa phải bị hỏng).

---
*Tài liệu dựa trên project thực tế điều chỉnh từ Open405R.*
