# Terminator Design Document

**Course**: Computing Lab (CS69201) · Term Project  
**Project**: Terminator: A Custom Terminal Emulator and Shell in C++  

---

## 1. System Architecture

Terminator is designed with a strictly decoupled architecture: the **X11 GUI Front-End** manages window creation, user keyboard input, and graphical rendering, while the **Tab Shell Engines** manage process creation, pipes, command execution, and shell state.

```
                      +-----------------------------+
                      |      X11 GUI Front-End      |
                      |  (Window, Tab Bar, Screen)  |
                      +--------------+--------------+
                                     |
              +----------------------+----------------------+
              |                                             |
        [Thread Mode]                                 [Process Mode]
    pthread_create worker                          fork child worker
              |                                             |
              +--------------+------------------------------+
                             | Bidirectional IPC Pipes
                             v
                      +-----------------------------+
                      |      Tab Shell Engine       |
                      | (cd, pipes, multiWatch,     |
                      |  job control, parsing, etc) |
                      +--------------+--------------+
                                     |
                      +--------------v--------------+
                      |    POSIX Shared Memory      |  <-- Shared History
                      |  (shm_open + mmap + sem_t)  |
                      +-----------------------------+
```

---

## 2. X11 GUI and Event Loop Design

### 2.1 Non-Busy-Wait Event Loop
Terminator avoids 100% CPU utilization by avoiding busy-wait polling loops. The core event loop employs the `poll()` system call:
1. File descriptors collected:
   - The X11 connection socket: `ConnectionNumber(display)`.
   - The read-ends of output pipes for all active tabs (`from_tab_fd_`).
2. The loop sleeps inside `poll(pfds, count, timeout_ms)`:
   - When the X server has pending events (`POLLIN` on X11 socket), `XPending(display)` and `XNextEvent(display, &event)` process the events.
   - When any shell produces output (`POLLIN` on a tab pipe), the GUI reads non-blockingly and appends text to that tab's screen line buffer.
   - If a tab terminates or crashes (`POLLHUP`), the GUI safely reaps and closes the tab.

### 2.2 Graphical Interface Elements
- **Tab Bar**: Drawn along the top with coordinates `0, 0` to `width, 28`. Renders active tabs in the exact specification format: `[ 1: ~/Downloads ] [ 2: /tmp ] [ + ]`.
- **Text Rendering**: Mono-spaced font `9x15` (fallback to `fixed`). Character metrics (`char_width`, `char_height`, `char_ascent`) determine baseline positioning.
- **Color Scheme**: High-contrast modern palette:
  - Background: `#1e1e1e` (dark charcoal)
  - Foreground Text: `#d4d4d4` (light gray)
  - Prompt: `#50fa7b` (vibrant green)
  - Cursor: `#00bcd4` (cyan solid block)
  - Active Tab: `#3c3836`
- **Scrolling**: Backlog buffer maintains up to 5,000 lines per tab. Mouse wheel buttons (Button 4 for up, Button 5 for down) increment/decrement `scroll_offset`.

---

## 3. Execution Modes & Shared State Synchronization

### 3.1 Thread Mode (Default: `./terminator`)
- The terminal runs as a single process.
- Each new tab launches a POSIX worker thread via `pthread_create()`.
- Bidirectional pipes (`pipe()`) connect the GUI to the thread's shell engine.
- Memory is shared within the process address space.

### 3.2 Process Mode (`./terminator useProcess=True`)
- The parent process hosts the X11 GUI only.
- Each new tab executes `fork()` to spawn an independent process and establishes process group isolation (`setpgid(0, 0)`).
- The GUI communicates with tab processes through IPC pipes.
- Crashes in one tab do not affect other tabs or the GUI parent.

### 3.3 Shared History Synchronization
Both execution modes share a single synchronized history store using POSIX Shared Memory:
- **Shared Segment**: Allocated via `shm_open()` and mapped via `mmap()` using the `SharedHistoryLayout` struct.
- **Process-Shared Semaphore**: An embedded `sem_t lock` is initialized with `pshared = 1` via `sem_init(&shared_data->lock, 1, 1)`.
- **Mutual Exclusion**: Any tab wanting to read (`getAll()`, `get()`) or append (`add()`) calls `sem_wait(&shared_data->lock)` and `sem_post(&shared_data->lock)`.
- Commands entered in Tab 1 immediately become visible to Tab 2 through the shared memory array.

---

## 4. Shell Engine & Process Management

### 4.1 Built-in Commands vs. External Commands
Commands that modify shell process state are executed as built-ins:
- `cd [dir]`: Changes only the active tab's internal working directory (`cwd_`). Emits an OSC title update sequence back to the GUI.
- `exit`: Sends an exit signal to GUI to destroy the active tab.
- `history`: Formats and displays the shared memory command history.
- `jobs`: Polls child status and lists active/stopped background jobs.
- `fg [%id]`: Resumes stopped jobs in foreground using `kill(-pgid, SIGCONT)` and `waitpid()`.
- `bg [%id]`: Resumes stopped jobs in background.
- `source <file>`: Reads and evaluates commands sequentially.
- `multiWatch`: Runs parallel watched commands.

External commands run in a child created with `fork()` + `execvp()`. The shell waits for them or places them in background depending on the `&` token.

---

## 5. IPC, Redirection & Pipelines

### 5.1 Redirection (`<`, `>`, `>>`)
- Before forking, Terminator verifies input file existence via `stat()`. If missing, it outputs an error without spawning children.
- In the child process:
  - Input redirection: `open(file, O_RDONLY)` followed by `dup2(fd, STDIN_FILENO)`.
  - Output redirection: `open(file, O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC), 0666)` followed by `dup2(fd, STDOUT_FILENO)`.

### 5.2 Arbitrary Pipelines
- For $N$ commands, Terminator creates $N-1$ pipes via `pipe()`.
- Each command forks into a dedicated child process.
- All children are added to the same Process Group (`pgid = first_child_pid`).
- Child $i$ redirects `STDIN_FILENO` to `pipes[i-1][0]` and `STDOUT_FILENO` to `pipes[i][1]`.
- Parent and child processes systematically close unused pipe descriptors to prevent pipe descriptor leaks and deadlocks.

---

## 6. multiWatch Implementation

`multiWatch ["cmd1", "cmd2", ...]` runs all target commands simultaneously:
- Each command is launched in its own process group via `fork()` + `execl("/bin/sh", "sh", "-c", cmd)`.
- The child process redirects stdout and stderr to the write-end of a dedicated pipe.
- The parent monitors all pipe read-ends simultaneously using `poll()` with non-busy-wait timeout.
- When output is received or a child finishes, Terminator reaps it and formats the output with unix timestamp:
  ```text
  "cmd1", 1790860020:
  ----------------------------------------------------
  <command output>
  ----------------------------------------------------
  ```
- Every 2 seconds (`MULTIWATCH_INTERVAL_SEC`), completed commands automatically re-launch.
- Upon receiving `Ctrl+C`, the parent terminates every watched child with `kill(-pid, SIGTERM)` and returns prompt to the shell.

---

## 7. Data Structures & Algorithms

### 7.1 Autocomplete
- When cursor is on the first token: scans built-in list and all executable files across `PATH` directories via `opendir()` / `readdir()`.
- When cursor is on subsequent tokens: scans current directory for matching files and subdirectories.
- Calculates longest common prefix across matches to partially expand input if multiple options exist.

### 7.2 Multiline & Quoting Parser
- Uses a deterministic state scanner tracking:
  - Quote state (`in_single_quote`, `in_double_quote`, `in_backtick`)
  - Escape state (`escaped`)
  - Subshell depth stack for nested `$(...)`
- Distinguishes literal single quotes from double quotes that expand variables.

---

## 8. Signal Handling & Job Control

- **Event-Driven Signals**: Keys like `Ctrl+C` and `Ctrl+Z` are captured as X11 events by the GUI. The shell dispatches them directly using `kill(-pgid, SIGINT)` or `kill(-pgid, SIGTSTP)`.
- **Terminator Resilience**: Terminator never terminates upon `Ctrl+C` or `Ctrl+Z`. Only the foreground process group is affected.
- **Job Control Table**: `JobManager` tracks jobs by PGID, child PIDs, command text, and state (`RUNNING`, `STOPPED`). Updates non-blockingly via `waitpid(-1, &status, WNOHANG | WUNTRACED | WCONTINUED)`.

