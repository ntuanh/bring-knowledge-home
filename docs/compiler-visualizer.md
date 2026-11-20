# Compiler Visualizer

Học compiler bằng cách sửa source, xem từng pha và đối chiếu kết quả với source.

```sh
make build
make run
```

Mở URL server in ra (mặc định tìm port trống trong 8100–8999). Cần Python 3,
GCC, Make và GNU binutils trên Linux x86-64; không cần thư viện Python ngoài.

- **C Compiler**: tokens, cây AST, scopes, IR, tối ưu, assembly, GCC/ELF inspector và chạy chương trình.
- **Compiler Lab**: giao diện chung cho compiler adapters; Back/Next, thanh chọn bước,
  highlight source và snapshot từng pha. Có Mini C và compiler biểu thức độc lập.
- **AI Compiler**: backend MiniTensor đã có parser, graph, fusion, loop IR và C codegen;
  tab giao diện hiện còn là placeholder.

Trong Compiler Lab, chọn **Mini C**, bật **O1**, rồi compile để xem các biến đổi
before/after của optimizer. Chọn **Expression compiler** và nhập `2 + 3 * (4 - 1)`
để xem token, AST, stack bytecode và constant folding ra `PUSH 11`.

Đây là trình xem lại dữ liệu sau compile. AST/IR của Mini C được duyệt từ snapshot;
optimizer dùng log biến đổi thực tế. Chưa có breakpoint hoặc dừng/tiếp tục tiến trình compiler.

Tích hợp compiler khác: [hợp đồng adapter và hướng dẫn](compiler-adapters.md).
Chỉ đổi source compiler là chưa đủ: compiler hoặc adapter phải xuất các pha và
trace mà bạn muốn quan sát theo protocol chung.

```sh
make test
```

Các test mới cho Compiler Lab nằm ở `visualizer/tests/test_adapters.py`.
