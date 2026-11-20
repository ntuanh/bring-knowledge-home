# Compiler adapters v1

Browser → `POST /api/compiler/compile` → adapter → compiler → session JSON → renderer chung.
`GET /api/compilers` liệt kê compiler được cấu hình. Hai API C/AI cũ vẫn hoạt động.

## Thêm compiler

Viết một executable hoặc wrapper nhận đường dẫn source và in **một JSON object**
ra stdout. Dùng stderr cho log. Compiler có lỗi source có thể exit 1 nhưng vẫn
phải trả session với trạng thái fail và dữ liệu các pha đã chạy.

Tạo `my-compilers.json` (đường dẫn executable nên là tuyệt đối):

```json
[
  {
    "id": "my-compiler",
    "name": "My compiler",
    "description": "My language → IR → target",
    "source": "2 + 3 * 4",
    "command": ["{python}", "{root}/visualizer/examples/expression_compiler.py", "{source}"]
  }
]
```

```sh
COMPILER_ADAPTERS="$PWD/my-compilers.json" make run
```

Mẫu trên đăng ký compiler biểu thức dưới ID mới. Thay `command` bằng compiler của
bạn hoặc wrapper chuyển output hiện có sang protocol dưới đây. Thay đổi compiler
không cần sửa JS hay routes. `id` phải duy nhất; `minic` và `expression` có sẵn.

Placeholder: `{python}` là Python đang chạy server, `{root}` là project root,
`{source}` là file UTF-8 tạm của request, `{opt}` là 0 hoặc 1. Không dùng shell;
mỗi phần tử của command là một argument. CWD là thư mục tạm và bị xoá sau request.
Đường dẫn source không chứa code source. Browser chỉ chọn ID đã đăng ký, không
gửi command. File cấu hình là code tin cậy do người vận hành server quản lý.

## Session

```json
{
  "version": 1,
  "stages": [
    {
      "id": "lower",
      "label": "IR lowering",
      "help": "Mỗi event ghi một lệnh được sinh từ source.",
      "status": "ok",
      "output": "PUSH 2\nPUSH 3\nADD",
      "events": [
        {
          "label": "Sinh PUSH 2",
          "location": {"line": 1, "col": 1, "endCol": 2},
          "data": {"instruction": "PUSH 2"}
        }
      ]
    }
  ]
}
```

- `stages`: danh sách không rỗng theo thứ tự pha; ID không trùng; label/help là chuỗi không rỗng.
- `status`: `ok`, `fail`, `skip`. Compiler tự xác định trạng thái. `skip` cũng dùng cho pha bị tắt.
- `output`: snapshot cuối pha, text hoặc JSON tuỳ compiler.
- `events`: các bước theo thứ tự, có thể bỏ qua hoặc để rỗng. UI dùng một bước snapshot nếu không có event.
- `label`: mô tả bước; `data` chứa trạng thái, instruction hoặc before/after.
- `location`: tuỳ chọn, vị trí trong source người dùng; line/col bắt đầu từ 1,
  endLine/endCol là biên kết thúc không bao gồm ký tự đó. Chỉ có line thì highlight cả dòng.

Source tối đa 64 KiB, output adapter tối đa 32 MiB, tối đa 20.000 events mỗi phiên,
timeout adapter 5 giây. Timeout, output bị cắt và JSON/schema sai trả HTTP 400.
Backend gắn `compiler: {id, name}` vào kết quả sau validation.

## Dữ liệu cần instrument

Wrapper có thể chuyển snapshots hiện có thành các bước duyệt như adapter Mini C
trong `visualizer/adapters.py`. Để thấy parser shift/reduce, symbol lookup hoặc
thứ tự chạy pass thật, cần instrument ngay trong compiler và ghi events tại đó.
Mô tả rõ thứ tự bước trong `help` để người học phân biệt trace thật với duyệt kết quả.

`visualizer/examples/expression_compiler.py` là compiler mẫu độc lập, không import
visualizer. Nó dùng Python parser để dựng AST cho tập con số nguyên và `+ - *`,
tự sinh stack bytecode và log constant folding. Adapter Mini C chỉ compile đến
assembly trong Lab; chạy chương trình và xem ELF nằm ở tab C Compiler.

Đây là replay sau compile, chưa phải debugger dừng process đang chạy. Hướng tiếp
theo là instrumentation lexer/parser, cây CFG, trạng thái từng pass, và sau đó
protocol streaming cho breakpoint/step thực sự.
