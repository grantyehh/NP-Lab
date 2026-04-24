# np_simple.c 筆記

## 程式碼整體架構

### 1. `main`
- 接收 port 參數，格式是 `./np_simple <port>`
- 設定 `SIGCHLD` handler，使用 `reap_children()` 回收 child，避免 zombie process
- 呼叫 `create_server_socket()` 建立 listen socket
- 進入 `while` 迴圈反覆 `accept()`
- 每當有新 client 連進來就 `fork()`
- parent 繼續 `accept()` 下一個 client
- child 會把 socket 導到自己的 stdin/stdout/stderr，然後進入 `run_shell_session()`

### 2. `create_server_socket`
- 建立 TCP socket
- 設定 `SO_REUSEADDR`
- 將 socket bind 到 `INADDR_ANY + port`
- 呼叫 `listen()`
- 成功後回傳 `listen_fd`

### 3. `run_shell_session`
- 每個 client 各自執行一份 shell session
- 初始化：
  - `DeferredPipe deferred[MAX_DEFERRED]`
  - `deferred_count`
  - `current_slot`
- 設定初始 `PATH=bin:.`
- 關閉 stdout/stderr buffering
- 進入 shell 主迴圈：
  - 印出 prompt `% `
  - 讀一行輸入
  - 用 `parse_line()` 拆成 commands
  - 先處理 built-in command
  - 再判斷是否走 batched pipeline
  - 否則交給 `execute_commands()`
- 每處理完一整行，`current_slot += count_line_segments(...)`

## Parsing 與資料結構

### 4. `Command`
- 表示一個解析完的 command
- 內容包括：
  - `argv`
  - `argc`
  - `infile`
  - `outfile`
  - `append`
  - `pipe_mode`
  - `pipe_num`
  - `pipe_stderr`

### 5. `DeferredPipe`
- 用來保存還沒被消費的 numbered pipe
- 內容包括：
  - `target_slot`
  - `read_fd`
  - `write_fd`

### 6. `ByteBuffer`
- batched pipeline 用的動態 buffer
- 內容包括：
  - `data`
  - `len`
  - `cap`

### 7. `parse_line`
- 用 `strtok(..., " ")` 逐個切 token
- 支援以下符號：
  - `<`
  - `>`
  - `>>`
  - `|`
  - `!`
  - `|N`
  - `!N`
- 還沒遇到 pipe/redirection 前的 token 都會放進 `current.argv`
- 一旦遇到 pipe 類 token，就把目前 `current` 存進 `commands[]`
- 行尾若還有殘留 command，也會補進 `commands[]`

## Numbered Pipe / Slot 邏輯

### 8. `count_line_segments`
- 不是單純算 command 數
- 一行預設至少有 `1` 個 segment / slot
- 每遇到一個 `PIPE_NUMBERED`，segment 就再加 `1`
- ordinary pipe 不會增加 segment

### 9. `get_or_create_deferred_pipe`
- 依 `target_slot` 找 deferred pipe
- 如果同一個 `target_slot` 已經存在，就直接重用
- 不存在才新建 unnamed pipe
- 成功後更新 `deferred_count`

### 10. `remove_deferred_pipe`
- 從 deferred array 中移除某一筆
- 後面的元素往前補
- `deferred_count--`

### 11. `collect_ready_deferred_inputs`
- 查看有哪些 deferred pipe 的 `target_slot == 目前 command 的 slot`
- 如果有，表示這些 pipe 已經準備好作為這個 command 的 stdin
- 會把它們的 `read_fd` 收進 `ready_fds[]`
- 同時把這些 deferred pipe 從表中移除

## Batched Pipeline

### 12. `parse_positive_env`
- 讀環境變數並轉成正整數
- 如果格式不合法就回傳 fallback

### 13. `get_fallback_threshold`
- 讀目前系統 process limit
- 推估一個安全的 pipeline batch threshold
- 也可被 `NP_PIPE_BATCH_THRESHOLD` 覆蓋

### 14. `get_batch_size`
- 回傳每批最多跑幾個 command
- 可被 `NP_PIPE_BATCH_SIZE` 覆蓋

### 15. `should_use_batched_pipeline`
- 判斷某一行是否要用 batched 模式
- 只有在 command 數量很多時才考慮
- 還需要滿足：
  - 全部 command 都有效
  - 沒有 infile/outfile/append
  - 沒有 `pipe_stderr`
  - 沒有 built-in
  - 中間只能是 ordinary pipe
  - 最後一個 command 不能再接 pipe

### 16. `free_buffer`
- 釋放 `ByteBuffer` 的記憶體

### 17. `ensure_buffer_capacity`
- 確保 `ByteBuffer` 容量足夠
- 不夠就用 `realloc()` 擴充

### 18. `append_buffer`
- 把一段資料追加到 `ByteBuffer`

### 19. `read_fd_to_buffer`
- 從某個 `fd` 持續 `read()`
- 把讀到的內容全部放進 `ByteBuffer`
- 在 batched pipeline 中，通常用來把這一批最後輸出的 `capture_pipe[0]` 收進 buffer

### 20. `run_pipeline_batch`
- 真正執行某一批 ordinary-pipe commands
- 如果有上一批輸出，就透過 `input_pipe` 把 `ByteBuffer input` 餵進第一個 command
- 最後一個 command 的 stdout 不直接印到螢幕，而是導到 `capture_pipe`
- 最後用 `read_fd_to_buffer()` 把 `capture_pipe[0]` 全部收進 `output`

### 21. `execute_commands_batched`
- 將很長的一整串 pipeline 分批處理
- 每批交給 `run_pipeline_batch()`
- 上一批的輸出存在 `ByteBuffer current`
- 下一批把 `current` 當作輸入
- 全部批次做完後，最後再把結果寫到 `STDOUT`

## 一般執行流程

### 22. `close_if_open`
- 小工具函式
- 如果 fd >= 0 就關閉
- 用來避免 fd leak

### 23. `write_all`
- 確保資料完整寫到指定 fd
- 避免一次 `write()` 沒寫完

### 24. `execute_commands`
- 一般模式下的核心執行器
- 大致流程：

#### A. 計算等待策略
- 算出 `last_segment_slot`
- 看最後一個 command 是否為 numbered pipe
- 如果最後 segment 需要真的完成，就記錄哪些 pid 必須 wait

#### B. 決定 stdin 來源
- 可能來源有三種：
  - 前一個 ordinary pipe 的 `prev_read_fd`
  - 已 ready 的 deferred pipe
  - `< infile`
- 若同時有多個 ready pipe，會 fork 一個 merge process，把多個輸入合併成同一條 pipe，再給 command 當 stdin

#### C. 決定 stdout/stderr 去向
- ordinary pipe：接到 `ordinary_pipe[1]`
- numbered pipe：接到 deferred pipe 的 `write_fd`
- `!` / `!N`：除了 stdout，也把 stderr 接到同一條 pipe
- `>` / `>>`：輸出到檔案

#### D. fork + exec
- child 裡面負責：
  - `dup2()` 接好 stdin/stdout/stderr
  - 關閉不必要 fd
  - `execvp()`
- 若 command 不存在，印：
  - `Unknown command: [cmd].`

#### E. parent 收尾
- 關閉不需要的 fd
- 更新 `prev_read_fd`
- 如果當前 command 是 `PIPE_NUMBERED`，就 `current_line++`
  - 代表同一行中後面的 command 已經進到下一個 slot

#### F. wait 策略
- 如果最後 segment 需要完成，就 wait 那些被標記的重要 pid
- 如果最後 segment 是 numbered pipe，就只做非阻塞回收，避免卡住 shell

## Server 輔助函式

### 25. `reap_children`
- `SIGCHLD` handler
- 用 `waitpid(-1, NULL, WNOHANG)` 持續回收已結束 child
- 防止 server 端累積 zombie processes

## 補充理解

### 26. slot / segment 觀念
- shell 不是單純依行數算 numbered pipe
- 也不是單純依 command 數量算
- 目前邏輯是依 `slot`
- 規則：
  - ordinary pipe 不切 slot
  - numbered pipe 會切 slot
  - 換行後，`current_slot` 一次跳過這一整行消耗掉的 segment 數

### 27. deferred_count 的意思
- `deferred[]` 陣列裡目前有幾筆有效的 deferred pipe
- 不是 pipe 編號
- 也不是 countdown
- 只是目前表中有效元素數量
