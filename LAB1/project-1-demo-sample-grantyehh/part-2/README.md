# NP Project 1 Part 2 (Concurrent Servers) Demo Script
## Important information:

1. This is a sample demo scipt. It is just a sample, the final one might be different.

2. The test cases in this directory are from 2016, the ones used for this year will be different.

3. Please make your servers fit the requirements:
   - Your np_simple takes in 1 argument, which is the port number.

   `./np_simple 12345`

   - The initial working directory structure:

```
    work_dir
        |-bin
        |-test.html
```

4. You can use the port that './port.py' outputs to test `reuseaddr` function.

5. We recommand you use telnet during delopment.
   - Assume you run your server on nplinux2 listening to port 12345
   - Run the following telnet commnad on any computer

```
    bash$ telnet nplinux1.cs.nycu.edu.tw 7001
    % ls | cat
    bin test.html
    % ls |1
    % cat
    bin test.html
    % exit
    bash$

```

6. Please be aware that the line sent from telnet and our delayclient might end with "\r\n".

7. The output order of the command "ls" might be different, we will try to avoid this situation during demo.


## This directory contains the following

### Server

- server.sh
    - Usage: `./server.sh <server_path> <port>`
    - server.sh does the following:
        1. Contruct the working directory (work_template and work_dir)
        2. Compile the commands (noop, removetag...) and place them into bin/ inside the working directory
        3. Copy cat and ls into bin/ inside the working directory
        4. Create working dir
        5. Run the your server inside working dir

- zombie.sh
    - Check if your server left any zombie processes in the system
    - Run this script right before you shut down your server

- src/
    - Contains source code of commands (noop, removetag...) and test.html

### Client

- single_client/ to test the np server (np_simple)
    
- demo.sh:
    - Usage: `./demo.sh <server ip> <port>`
    - demo.sh does the following:
        1. Test all testcases using delayclient
        2. Use diff to compare the files inside output/ and answer/
        3. Show the result of demo

- compare.sh
    - Usage: `./compare.sh [n]`
    - Compare.sh will run vimdiff on the n'th answer and your output

- test_case/
    - Contains test cases

- answer/
    - Contains answers

## Usage

### Tmux usage

- create tmux: `tmux`
- enter exist tmux session: `tmux a`
- exit but not kill session: `Ctrl-b + d`
- create new window: `Ctrl-b + c`
- next window: `Ctrl-b + n`
- kill window: `Ctrl-b + &`
- exit a pane: `Ctrl-d` (send EOF to shell)
- scroll screen: `Ctrl-b + [` (quit with q)
- go to window up: `Ctrl-b + <up>`

### Testing one server

```
./demo.sh <server_path> <port>

example:
./demo.sh ../np_simple 12345
```

Create two panes and run server with corresponding testcases. You can `cd` into corresponding client and run `compare.sh` on the top pane.

```
[yywu7856443@nplinux12 ~]$















────────────────────────────────────────────────────────────────────┬────────────────────────────────────────────────────────────────
[yywu7856443@nplinux12 multi_client_1]$ ./demo.sh 127.0.0.1 12344   │rm -rf work_dir
===== Test case 1 =====                                             │rm -rf work_template
Your answer is correct                                              │mkdir work_template
===== Test case 2 =====                                             │cp src/file/test.html work_template/test.html
Your answer is correct                                              │mkdir work_template/bin/
===== Test case 3 =====                                             │clang++ src/cmd/number.cpp -Wall -O2 -pedantic -std=c++11 -o wor
Your answer is correct                                              │k_template/bin/number
===== Test case 4 =====                                             │clang++ src/cmd/removetag.cpp -Wall -O2 -pedantic -std=c++11 -o
Your answer is correct                                              │work_template/bin/removetag
===== Test case 5 =====                                             │clang++ src/cmd/noop.cpp -Wall -O2 -pedantic -std=c++11 -o work_
Your answer is correct                                              │template/bin/noop
===== Test case 6 =====                                             │clang++ src/cmd/removetag0.cpp -Wall -O2 -pedantic -std=c++11 -o
Your answer is correct                                              │ work_template/bin/removetag0
===== Test case 7 =====                                             │cp -f /bin/ls work_template/bin/ls
Your answer is correct                                              │cp -f /bin/cat work_template/bin/cat
======= Summary =======                                             │mkdir work_template/user_pipe
[Correct]: 1 2 3 4 5 6 7                                            │======= Your server is running =======
[yywu7856443@nplinux12 multi_client_1]$                             │
```



