set pagination off
set confirm off

set $SEU_POW2 = 7
set $BIT_MAX  = 31

target remote :1234
monitor reset halt
load

set $flip_cnt = 0

define maybe_inject
  set $seq = (unsigned int)$arg0
  
  set $salt = (unsigned int)($SEU_MODE) * 0x9E3779B1u
  set $r = (unsigned int)(($seq ^ $salt) * 1103515245u + 12345u)
  
  set $mask = (1u << (unsigned int)$SEU_POW2) - 1u
  
  if (($r & $mask) == 0u)
    set $bit = (int)(($r >> 8) & 31u)
    set $bit = $bit % ((int)$BIT_MAX + 1)
    
    set $current_val = *(uint32_t*)$arg1
    set $new_val = $current_val ^ (1u << $bit)
    set *(uint32_t*)$arg1 = $new_val
    
    set $flip_cnt = $flip_cnt + 1
    printf "[GDB-SEU] seq=%u flip bit=%d flips=%d\n", $seq, $bit, $flip_cnt
  end
end

# SEU_MODE = 0 => flip prev
break seu_hook_prev
commands
  silent

  if ($SEU_MODE != 0)
    continue
  end
  
  set $curr_seq = (unsigned int)curr_used->seq
  if ($curr_seq == 0u)
    continue
  end
  
  set $seq = $curr_seq
  set $r2 = (unsigned int)($seq * 1664525u + 1013904223u)
  set $axis = (int)($r2 % 3u)

  if ($axis == 0)
    maybe_inject $seq &(prev->bx)
  end
  if ($axis == 1)
    maybe_inject $seq &(prev->by)
  end
  if ($axis == 2)
    maybe_inject $seq &(prev->bz)
  end
  
  continue
end

# SEU_MODE = 1 => flip curr (bez TMR)
break seu_hook_curr
commands
  silent
  if ($SEU_MODE != 1)
    continue
  end
  
  set $seq = (unsigned int)curr_used->seq
  if ($seq == 0u)
    continue
  end
  
  set $r2 = (unsigned int)($seq * 1664525u + 1013904223u)
  set $axis = (int)($r2 % 3u)

  if ($axis == 0)
    maybe_inject $seq &(curr_used->bx)
  end
  if ($axis == 1)
    maybe_inject $seq &(curr_used->by)
  end
  if ($axis == 2)
    maybe_inject $seq &(curr_used->bz)
  end
  
  continue
end

# SEU_MODE = 1 (TMR) => flip replica
break seu_hook_curr_tmr
commands
  silent
  if ($SEU_MODE != 1)
    continue
  end
  
  set $seq = (unsigned int)r0->seq
  if ($seq == 0u)
    continue
  end
  
  # Wybór repliki
  set $rR = (unsigned int)($seq * 1664525u + 1013904223u)
  set $rep = (int)($rR % 3u)
  
  # Wybór osi
  set $rA = (unsigned int)($seq * 22695477u + 1u)
  set $axis = (int)($rA % 3u)
  
  set $t = r0
  if ($rep == 1)
    set $t = r1
  end
  if ($rep == 2)
    set $t = r2
  end
  
  if ($axis == 0)
    maybe_inject $seq &($t->bx)
  end
  if ($axis == 1)
    maybe_inject $seq &($t->by)
  end
  if ($axis == 2)
    maybe_inject $seq &($t->bz)
  end
  
  continue
end

# SEU_MODE = 2 => flip cmd
break seu_hook_cmd
commands
  silent
  if ($SEU_MODE != 2)
    continue
  end
  
  set $seq = (unsigned int)cmd->seq
  if ($seq == 0u)
    continue
  end
  
  set $r2 = (unsigned int)($seq * 1103515245u + 12345u)
  set $axis = (int)($r2 % 3u)

  if ($axis == 0)
    maybe_inject $seq &(cmd->mx)
  end
  if ($axis == 1)
    maybe_inject $seq &(cmd->my)
  end
  if ($axis == 2)
    maybe_inject $seq &(cmd->mz)
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