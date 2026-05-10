# MasterBandit shell integration bootstrap (zsh).
#
# At spawn time, mb sets ZDOTDIR to this directory (saving any prior
# value in MB_ORIG_ZDOTDIR). zsh sources THIS .zshenv first; we restore
# the user's real ZDOTDIR (so subsequent .zshrc/.zlogin/.zprofile come
# from there), source the user's real .zshenv ourselves (zsh has already
# moved past that step), then load the OSC 133 / OSC 7 / OSC 2 hooks.
#
# The hook install runs inside `always { ... }` so a syntax error in the
# user's .zshenv doesn't skip integration.

# Resolve our own directory. $0 is unreliable: when zsh auto-sources
# .zshenv during shell startup, $0 is the shell binary path (e.g.
# /bin/zsh), not the script. The %x prompt-expansion code always gives
# the path of the file currently being sourced, which is what we want.
typeset _mb_dir=${${(%):-%x}:A:h}

() {
    builtin emulate -L zsh -o no_aliases

    # Restore ZDOTDIR. The MB_ORIG_ZDOTDIR marker lets us distinguish
    # "user had ZDOTDIR set before mb spawned" (restore it) from "user
    # didn't" (unset it, so $HOME wins like normal).
    if [[ -n "${MB_ORIG_ZDOTDIR-}" ]]; then
        builtin export ZDOTDIR=$MB_ORIG_ZDOTDIR
        builtin unset MB_ORIG_ZDOTDIR
    else
        builtin unset ZDOTDIR
    fi

    {
        # Source the user's real .zshenv. zsh sourced ours instead of
        # theirs because we hijacked ZDOTDIR; replay the step manually.
        local user_zdotdir=${ZDOTDIR-$HOME}
        if [[ -r "$user_zdotdir/.zshenv" ]]; then
            builtin source "$user_zdotdir/.zshenv"
        fi
    } always {
        if [[ -r "$_mb_dir/mb-integration.zsh" ]]; then
            builtin source "$_mb_dir/mb-integration.zsh"
        fi
    }
}

builtin unset _mb_dir
