# Nie zatrzymuj się na przewijaniu długich outputów
set pagination off
# Nie pytaj o potwierdzenia
set confirm off

# SEU_POW2: kontrola częstości wstrzyknięć (1 na 2^SEU_POW2 próbek)
set $SEU_POW2 = 7
# Maksymalny indeks bitu do flipowania
set $BIT_MAX  = 31

# Podłącz się do zdalnego targetu GDB
target remote :1234
# Zresetuj CPU i zatrzymaj na starcie
monitor reset halt
# Wgraj firmware do pamięci
load

# Licznik wykonanych flipów
set $flip_cnt = 0

# Z pewnym prawdopodobieństwem flipuj 1 losowy bit w *addr
define maybe_inject
  # Prosty PRNG (LCG)
  set $seq = (unsigned int)$arg0
  set $salt = (unsigned int)($SEU_MODE) * 0x9E3779B1u
  set $r = (unsigned int)(($seq ^ $salt) * 1103515245u + 12345u)
  
  # Maska decyzji czy wstrzyknąć
  set $mask = (1u << (unsigned int)$SEU_POW2) - 1u
  
  # Wstrzyknięcie tylko gdy trafi się r & mask == 0
  if (($r & $mask) == 0u)
    set $bit = (int)(($r >> 8) & 31u)
    set $bit = $bit % ((int)$BIT_MAX + 1)
    
    # Flip jednego bitu
    set $current_val = *(uint32_t*)$arg1
    set $new_val = $current_val ^ (1u << $bit)
    # Zapisz do pamięci
    set *(uint32_t*)$arg1 = $new_val
    
    set $flip_cnt = $flip_cnt + 1
    printf "[GDB-SEU] seq=%u flip bit=%d flips=%d\n", $seq, $bit, $flip_cnt
  end
end

# SEU_MODE = 0, flip w input_prev
break seu_hook_input_prev
commands
  silent

  if ($SEU_MODE != 0)
    continue
  end
  
  set $input_curr_seq = (unsigned int)input_curr_used->seq
  if ($input_curr_seq == 0u)
    continue
  end
  
  set $seq = $input_curr_seq
  # Drugi PRNG do wyboru osi (X/Y/Z) niezależny od maybe_inject()
  set $r2 = (unsigned int)($seq * 1664525u + 1013904223u)
  set $axis = (int)($r2 % 3u)

  if ($axis == 0)
    maybe_inject $seq &(input_prev->bx)
  end
  if ($axis == 1)
    maybe_inject $seq &(input_prev->by)
  end
  if ($axis == 2)
    maybe_inject $seq &(input_prev->bz)
  end
  
  continue
end

# SEU_MODE = 1, flip w input_curr
break seu_hook_input_curr
commands
  silent

  if ($SEU_MODE != 1)
    continue
  end
  
  set $seq = (unsigned int)input_curr_used->seq
  if ($seq == 0u)
    continue
  end
  
  # Wybór osi (X/Y/Z)
  set $r2 = (unsigned int)($seq * 1664525u + 1013904223u)
  set $axis = (int)($r2 % 3u)

  if ($axis == 0)
    maybe_inject $seq &(input_curr_used->bx)
  end
  if ($axis == 1)
    maybe_inject $seq &(input_curr_used->by)
  end
  if ($axis == 2)
    maybe_inject $seq &(input_curr_used->bz)
  end
  
  continue
end

# SEU_MODE = 1 (TMR), flip w jednej z trzech replik (r0/r1/r2)
break seu_hook_input_curr_tmr
commands
  silent

  if ($SEU_MODE != 1)
    continue
  end
  
  set $seq = (unsigned int)r0->seq
  if ($seq == 0u)
    continue
  end
  
  # Wybór repliki 0..2
  set $rR = (unsigned int)($seq * 1664525u + 1013904223u)
  set $rep = (int)($rR % 3u)
  
  # Wybór osi 0..2
  set $rA = (unsigned int)($seq * 22695477u + 1u)
  set $axis = (int)($rA % 3u)
  
  set $t = r0
  if ($rep == 1)
    set $t = r1
  end
  if ($rep == 2)
    set $t = r2
  end
  
  # Wstrzyknij SEU w jedną oś wybranej repliki
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

# SEU_MODE = 2, flip w output_cmd
break seu_hook_output_cmd
commands
  silent

  if ($SEU_MODE != 2)
    continue
  end
  
  set $seq = (unsigned int)output_cmd->seq
  if ($seq == 0u)
    continue
  end
  
  # Wybór osi (X/Y/Z) dla komendy
  set $r2 = (unsigned int)($seq * 1103515245u + 12345u)
  set $axis = (int)($r2 % 3u)

  # Wstrzyknij SEU w jedną oś output_cmd->m*
  if ($axis == 0)
    maybe_inject $seq &(output_cmd->mx)
  end
  if ($axis == 1)
    maybe_inject $seq &(output_cmd->my)
  end
  if ($axis == 2)
    maybe_inject $seq &(output_cmd->mz)
  end
  
  continue
end

# Breakpoint na końcu eksperymentu
break end_hook
commands
  silent
  printf "\n[GDB] SEU_MODE=%d flips=%d\n", (int)$SEU_MODE, (int)$flip_cnt
  continue
end

# Start wykonania programu
continue
