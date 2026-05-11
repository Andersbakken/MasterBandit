# MasterBandit OSC 133 / OSC 7 / OSC 2 hooks for zsh.
#
# Hooks (via add-zsh-hook so user hooks coexist):
#   precmd   -> emit 133;D;<exit> for the previous command (if any),
#               then 133;A (prompt-start) and OSC 7 cwd.
#   preexec  -> emit 133;C (output starts).
# 133;B (command-start, where typed input begins) is appended to PS1 so
# it lands at the very end of the rendered prompt, regardless of theme.
#
# Feature flags (opt-out, space-separated in MB_SHELL_INTEGRATION):
#   no-prompt-mark  -> skip 133;A/B/C/D entirely
#   no-cwd          -> skip OSC 7
#   no-title        -> skip OSC 2 title re-emit (currently unused — will
#                      apply once a configurable title format lands)

# Non-interactive shells never reach a prompt; nothing to wire up.
[[ -o interactive ]] || return 0

# Already loaded? Re-sourcing (e.g. user-driven) shouldn't double-register.
[[ -n "${_MB_INTEGRATION_LOADED-}" ]] && return 0
typeset -g _MB_INTEGRATION_LOADED=1

# Parse feature flags once into an associative array.
typeset -gA _mb_features
() {
    local f
    for f in ${=MB_SHELL_INTEGRATION-}; do
        _mb_features[$f]=1
    done
}

_mb_disabled() {
    [[ -n ${_mb_features[no-$1]-} ]]
}

autoload -Uz add-zsh-hook

# State: are we currently between a 133;C and the next 133;D?
typeset -g _mb_in_command=0

# OSC sequences. BEL terminator (\a) — broader compatibility than ST
# (\e\\) across older zsh and tmux passthrough modes.

_mb_precmd() {
    # Capture exit status FIRST — anything else clobbers $?.
    local exit_code=$?

    if _mb_disabled prompt-mark; then
        :
    else
        if (( _mb_in_command )); then
            builtin print -nu1 $'\e]133;D;'$exit_code$'\a'
            _mb_in_command=0
        fi
        builtin print -nu1 $'\e]133;A\a'
    fi

    if ! _mb_disabled cwd; then
        # OSC 7 cwd: file://<host><url-encoded-path>. Encode anything
        # outside the unreserved set; "/" is a separator and stays literal.
        # NB: don't name the local "path" — that's the array form of $PATH
        # in zsh and assigning to it clobbers PATH (and breaks ${#var}).
        local hostname=${HOST:-localhost}
        local cwd=$PWD
        local encoded='' i ch
        for (( i = 1; i <= ${#cwd}; i++ )); do
            ch=$cwd[i]
            case $ch in
                [A-Za-z0-9._~/-]) encoded+=$ch ;;
                *) encoded+=$(builtin printf '%%%02X' "'$ch") ;;
            esac
        done
        builtin print -nu1 $'\e]7;file://'"${hostname}${encoded}"$'\a'
    fi
}

_mb_preexec() {
    if ! _mb_disabled prompt-mark; then
        builtin print -nu1 $'\e]133;C\a'
    fi
    _mb_in_command=1
}

# Title re-emission. Many TUI apps (vim, less, htop) set a title via OSC 2
# but never restore it on exit. We unconditionally re-emit on every prompt
# cycle so a leaked title self-heals at the next prompt, and on preexec
# so the running command's name appears while it runs.
#
# Override _mb_title_for_prompt / _mb_title_for_command in your .zshrc to
# customize the format; the default is the tilde-abbreviated cwd / the
# first word of the command line.
_mb_title_for_prompt() {
    builtin print -r -- "${PWD/#$HOME/~}"
}

_mb_title_for_command() {
    # $1 is the expanded command line zsh passes to preexec hooks.
    local first=${1%% *}
    builtin print -r -- "$first"
}

_mb_emit_title() {
    _mb_disabled title && return
    # OSC 0 sets icon name AND window title — mirrors what most apps that
    # set a title actually do, so re-emit overwrites their leak in full.
    builtin print -nu1 $'\e]0;'$1$'\a'
}

_mb_precmd_title() {
    _mb_emit_title "$(_mb_title_for_prompt)"
}

_mb_preexec_title() {
    _mb_emit_title "$(_mb_title_for_command "$1")"
}

# Inject 133;B at end of PS1 each prompt cycle. Themes that rebuild PS1
# in their own precmd will (correctly) overwrite ours; we re-inject on
# the next cycle. Idempotent within a cycle via the substring check.
_mb_inject_b_into_prompt() {
    _mb_disabled prompt-mark && return
    [[ $PS1 == *$'\e]133;B'* ]] && return
    PS1+=$'%{\e]133;B\a%}'
}

# Order matters: _mb_precmd runs first to capture $? before any other
# precmd function clobbers it. Theme/plugin precmd hooks added by the
# user's .zshrc append after, which is fine.
add-zsh-hook -Uz precmd _mb_precmd
add-zsh-hook -Uz precmd _mb_inject_b_into_prompt
add-zsh-hook -Uz precmd _mb_precmd_title
add-zsh-hook -Uz preexec _mb_preexec
add-zsh-hook -Uz preexec _mb_preexec_title
