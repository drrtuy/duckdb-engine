#!/bin/bash

# ─── Color setup ────────────────────────────────────────────────────────────────

if [[ -n "$TERM" && "$TERM" != "dumb" ]] && command -v tput &>/dev/null; then
  _CLR_RESET=$(tput sgr0)
  _CLR_BOLD=$(tput bold)
  _CLR_RED="${_CLR_BOLD}$(tput setaf 1)"
  _CLR_GREEN="${_CLR_BOLD}$(tput setaf 2)"
  _CLR_YELLOW="${_CLR_BOLD}$(tput setaf 3)"
  _CLR_BLUE="${_CLR_BOLD}$(tput setaf 4)"
  _CLR_CYAN="${_CLR_BOLD}$(tput setaf 6)"
  _CLR_GRAY=$(tput setaf 7)
  _CLR_DARKGRAY="${_CLR_BOLD}$(tput setaf 0)"

  if [[ $(tput colors) -ge 256 ]]; then
    _CLR_RED=$(tput setaf 196)
    _CLR_GREEN=$(tput setaf 156)
    _CLR_YELLOW=$(tput setaf 228)
    _CLR_CYAN=$(tput setaf 87)
    _CLR_DARKGRAY=$(tput setaf 59)
  fi
else
  _CLR_RESET="\e[0m"
  _CLR_BOLD="\e[1m"
  _CLR_RED="\e[1;31m"
  _CLR_GREEN="\e[1;32m"
  _CLR_YELLOW="\e[1;33m"
  _CLR_BLUE="\e[1;34m"
  _CLR_CYAN="\e[1;36m"
  _CLR_GRAY="\e[37m"
  _CLR_DARKGRAY="\e[1;30m"
fi

# ─── Logging functions ──────────────────────────────────────────────────────────

info() {
  echo -e "${_CLR_CYAN} ● $*${_CLR_RESET}"
}

warn() {
  echo -e "${_CLR_YELLOW} ⚠ $*${_CLR_RESET}"
}

error() {
  echo -e "${_CLR_RED} ✖ $*${_CLR_RESET}"
}

success() {
  echo -e "${_CLR_GREEN} ✔ $*${_CLR_RESET}"
}

fail() {
  echo -e "${_CLR_RED} ✖ $*${_CLR_RESET}"
  exit 1
}

separator() {
  echo -e "${_CLR_DARKGRAY} ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${_CLR_RESET}"
}

header() {
  separator
  echo -e "${_CLR_GREEN} $*${_CLR_RESET}"
  separator
}

# ─── One-liner pipe ─────────────────────────────────────────────────────────────
# Usage: cmake --build . | one_liner
# Pipes stdout through a single updating line. Stderr goes straight to terminal.

one_liner() {
  local cols
  cols=$(tput cols 2>/dev/null || echo 120)
  while IFS= read -r line; do
    printf "\r\e[K  %s" "${line:0:$((cols - 4))}"
  done
  printf "\r\e[K"
}

# ─── Arrow-key menu ─────────────────────────────────────────────────────────────
# Usage:
#   options=("Debug" "RelWithDebInfo" "Release")
#   menu_choice "Select build type:" options
#   echo "Selected: $MENU_RESULT"
#
# Sets global MENU_RESULT to the chosen item string.

menu_choice() {
  local prompt="$1"
  local -n _opts=$2
  local count=${#_opts[@]}
  local selected=0

  if [[ $count -eq 0 ]]; then
    error "menu_choice: no options provided"
    return 1
  fi

  # Hide cursor, restore on exit/ctrl-c
  printf "\e[?25l"
  trap 'printf "\e[?25h"' RETURN
  trap 'printf "\e[?25h"; exit 130' INT

  _menu_render() {
    # Move up to overwrite previous render (skip on first draw)
    [[ ${1:-0} -gt 0 ]] && printf "\e[%dA" "$((count + 2))"

    echo -e "\n ${_CLR_CYAN}${prompt}${_CLR_RESET}"
    for ((i = 0; i < count; i++)); do
      if [[ $i -eq $selected ]]; then
        echo -e "   ${_CLR_GREEN}▸ ${_opts[$i]}${_CLR_RESET}"
      else
        echo -e "   ${_CLR_GRAY}  ${_opts[$i]}${_CLR_RESET}"
      fi
    done
  }

  _menu_render 0

  while true; do
    read -rsn1 key
    case "$key" in
      $'\x1b')
        read -rsn2 rest
        case "$rest" in
          '[A') # Up
            ((selected--))
            ((selected < 0)) && selected=$((count - 1))
            _menu_render 1
            ;;
          '[B') # Down
            ((selected++))
            ((selected >= count)) && selected=0
            _menu_render 1
            ;;
        esac
        ;;
      '') # Enter
        break
        ;;
    esac
  done

  # Clear the menu from terminal
  printf "\e[%dA" "$((count + 2))"
  for ((i = 0; i < count + 2; i++)); do
    printf "\e[K\n"
  done
  printf "\e[%dA" "$((count + 2))"

  printf "\e[?25h"
  echo -e " ${_CLR_CYAN}${prompt}${_CLR_RESET} ${_CLR_GREEN}${_opts[$selected]}${_CLR_RESET}"

  MENU_RESULT="${_opts[$selected]}"
}
