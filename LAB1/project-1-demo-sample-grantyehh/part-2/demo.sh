#!/bin/bash

PROJECT_PATH=$1
PORT1=$2

if [ -z "$PROJECT_PATH" ] || [ -z "$PORT1" ] || ([ -d "$PROJECT_PATH" ]); then
  printf "Usage: $0 <project_path> <port1>\n       $0 <server_path> <port>\n"
  exit 1
fi

if [ -z "$TMUX" ]; then
  if tmux ls | grep -q np_demo; then
    tmux kill-session -t np_demo
  fi
  tmux new-session -s np_demo -n np_demo_sample \
  "tmux split-window -v -p 95; tmux split-window -v -p 55; cd $PWD; ./demo_task.sh $PROJECT_PATH $PORT1 2 3"
fi
