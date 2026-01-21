set pagination off
set confirm off

set $SEU_MODE = 1
set $SEU_POW2 = 7
set $BIT_MAX = 15

target remote :1234
monitor reset halt
load

set $flip_cnt = 0

define maybe_inject
  set $seq = (unsigned)$arg0
  set $salt = (unsigned)($SEU_MODE) * 0x9E3779B1
  set $r = (unsigned)(($seq ^ $salt) * 1103515245 + 12345)

  set $mask = (1u << $SEU_POW2) - 1u

  if (($r & $mask) == 0)
    set $bit = (int)(($r >> 8) & 31u)
    set $bit = $bit % (int)($BIT_MAX + 1)

    set var $arg1 = (uint32_t)$arg1 ^ (1u << $bit)

    set $flip_cnt = (int)$flip_cnt + 1
    printf "[GDB-SEU] seq=%u %s flip bit=%d flips=%d\n", $seq, $arg2, $bit, $flip_cnt
  end
end

break seu_hook_prev
commands
  silent
  if ($SEU_MODE != 0)
    continue
  end
  if ((unsigned)curr->seq == 0u)
    continue
  end

  set $r2 = (unsigned)(curr->seq * 1664525u + 1013904223u)
  set $axis = (int)($r2 % 3u)

  if ($axis == 0)
    maybe_inject curr->seq prev->Bx "prev.Bx"
  end
  if ($axis == 1)
    maybe_inject curr->seq prev->By "prev.By"
  end
  if ($axis == 2)
    maybe_inject curr->seq prev->Bz "prev.Bz"
  end

  continue
end

break seu_hook_curr_tmr
commands
  silent
  if ($SEU_MODE != 1)
    continue
  end

  set $seq = (unsigned)c0->seq
  if ($seq == 0u)
    continue
  end

  set $rR = (unsigned)($seq * 1664525u + 1013904223u)
  set $rep = (int)($rR % 3u)

  set $rA = (unsigned)($seq * 22695477u + 1u)
  set $axis = (int)($rA % 3u)

  set $t = c0
  if ($rep == 1)
    set $t = c1
  end
  if ($rep == 2)
    set $t = c2
  end

  set $tag = "r0"
  if ($rep == 1)
    set $tag = "r1"
  end
  if ($rep == 2)
    set $tag = "r2"
  end

  if ($axis == 0)
    maybe_inject $seq $t->Bx $tag
  end
  if ($axis == 1)
    maybe_inject $seq $t->By $tag
  end
  if ($axis == 2)
    maybe_inject $seq $t->Bz $tag
  end

  continue
end

break seu_hook_cmd
commands
  silent
  if ($SEU_MODE != 2)
    continue
  end
  if ((unsigned)cmd->seq == 0u)
    continue
  end

  set $r2 = (unsigned)(cmd->seq * 1103515245u + 12345u)
  set $axis = (int)($r2 % 3u)

  if ($axis == 0)
    maybe_inject cmd->seq cmd->mx "cmd.mx"
  end
  if ($axis == 1)
    maybe_inject cmd->seq cmd->my "cmd.my"
  end
  if ($axis == 2)
    maybe_inject cmd->seq cmd->mz "cmd.mz"
  end

  continue
end


break end_hook
commands
  silent
  printf "\n[GDB] SEU_MODE=%d flips=%d\n", (int)$SEU_MODE, (int)$flip_cnt
  continue
end

continue
