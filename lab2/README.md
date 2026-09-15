# Отчёт по домашнему заданию: перехват SIGFPE на x86_64 и влияние UB-оптимизаций

**Среда:** Ubuntu 20.04.6 LTS, x86_64  
**Компиляторы:** gcc 9.4.0, clang 18.1.8  
**Флаги сборки:** `-O0 -g` (Debug), `-O3 -g` (Release)  
**Декодер инструкций:** Zydis 4.x (amalgamated)

---

## 1. Ассемблерные дампы, поиск инструкций для `if (b == 0)`

Дампы сняты командой:

`objdump -d -M intel --no-show-raw-insn <binary> | sed -n '/<main>:/,/^$/p'`

Ниже представлены фрагменты функции `main` для обоих тестов (сценарий A — volatile-делитель, сценарий B — делитель в локальной переменной) во всех четырёх комбинациях.

### 1.1. gcc `-O0`

A: `volatile int b`

```
00000000000112c9 <main>:
   112c9:	endbr64 
   112cd:	push   rbp
   112ce:	mov    rbp,rsp
   112d1:	sub    rsp,0x10
   112d5:	call   114ec <install_sigfpe_handler>
   112da:	mov    DWORD PTR [rbp-0x8],0x64
   112e1:	mov    eax,DWORD PTR [rbp-0x8]
   112e4:	mov    ecx,DWORD PTR [rip+0x95a82]        # a6d6c <b>
   112ea:	cdq    
   112eb:	idiv   ecx                              ## b
   112ed:	mov    DWORD PTR [rbp-0x4],eax
   112f0:	mov    eax,DWORD PTR [rbp-0x4]
   112f3:	mov    esi,eax
   112f5:	lea    rdi,[rip+0x1fd08]        # 31004 <_IO_stdin_used+0x4>
   112fc:	mov    eax,0x0
   11301:	call   11150 <printf@plt>
   11306:	mov    eax,DWORD PTR [rip+0x95a60]        # a6d6c <b>
   1130c:	test   eax,eax                          ## b == 0?
   1130e:	jne    1131e <main+0x55>
   11310:	lea    rdi,[rip+0x1fd09]        # 31020 <_IO_stdin_used+0x20>   "b is 0"
   11317:	call   11110 <puts@plt>
   1131c:	jmp    1132a <main+0x61>
   1131e:	lea    rdi,[rip+0x1fd06]        # 3102b <_IO_stdin_used+0x2b>   "b is NOT 0"
   11325:	call   11110 <puts@plt>
   1132a:	mov    eax,0x0
   1132f:	leave  
   11330:	ret    
```

B: `int b = bv`

```
0000000000112c9 <main>:
   112c9:	endbr64 
   112cd:	push   rbp
   112ce:	mov    rbp,rsp
   112d1:	sub    rsp,0x10
   112d5:	call   114ec <install_sigfpe_handler>
   112da:	mov    DWORD PTR [rbp-0xc],0x64
   112e1:	mov    eax,DWORD PTR [rip+0x95a85]        # a6d6c <bv>  volatile-read
   112e7:	mov    DWORD PTR [rbp-0x8],eax              ## b (local)
   112ea:	mov    eax,DWORD PTR [rbp-0xc]
   112ed:	cdq    
   112ee:	idiv   DWORD PTR [rbp-0x8]
   112f1:	mov    DWORD PTR [rbp-0x4],eax
   112f4:	mov    eax,DWORD PTR [rbp-0x4]
   112f7:	mov    esi,eax
   112f9:	lea    rdi,[rip+0x1fd04]        # 31004 <_IO_stdin_used+0x4>
   11300:	mov    eax,0x0
   11305:	call   11150 <printf@plt>
   1130a:	cmp    DWORD PTR [rbp-0x8],0x0      ## b == 0?
   1130e:	jne    1131e <main+0x55>
   11310:	lea    rdi,[rip+0x1fd09]        # 31020 <_IO_stdin_used+0x20>
   11317:	call   11110 <puts@plt>
   1131c:	jmp    1132a <main+0x61>
   1131e:	lea    rdi,[rip+0x1fd06]        # 3102b <_IO_stdin_used+0x2b>
   11325:	call   11110 <puts@plt>
   1132a:	mov    eax,0x0
   1132f:	leave  
   11330:	ret    
```

**Вывод:** при `-O0` ветка `if (b == 0)` физически присутствует в обоих сценариях. Инструкции test/cmp + jne сгенерированы - оптимизации отключены.

### 1.2. gcc `-O3`

A: `volatile int b`

```
0000000000011200 <main>:
   11200:	endbr64 
   11204:	sub    rsp,0x18
   11208:	call   11480 <install_sigfpe_handler>
   1120d:	mov    DWORD PTR [rsp+0xc],0x64
   11215:	mov    eax,DWORD PTR [rsp+0xc]
   11219:	lea    rsi,[rip+0x1cde4]        # 2e004 <_IO_stdin_used+0x4>
   11220:	mov    ecx,DWORD PTR [rip+0x91e06]        # volatile read
   11226:	mov    edi,0x1
   1122b:	cdq    
   1122c:	idiv   ecx                                # volatile read
   1122e:	mov    edx,eax
   11230:	xor    eax,eax
   11232:	call   111a0 <__printf_chk@plt>
   11237:	mov    eax,DWORD PTR [rip+0x91def]        # a302c <b>
   1123d:	test   eax,eax
   1123f:	jne    11254 <main+0x54>
   11241:	lea    rdi,[rip+0x1cdd8]        # 2e020 <_IO_stdin_used+0x20>
   11248:	call   11110 <puts@plt>
   1124d:	xor    eax,eax
   1124f:	add    rsp,0x18
   11253:	ret    
   11254:	lea    rdi,[rip+0x1cdd0]        # 2e02b <_IO_stdin_used+0x2b>
   1125b:	call   11110 <puts@plt>
   11260:	jmp    1124d <main+0x4d>
   11262:	nop    WORD PTR cs:[rax+rax*1+0x0]
   1126c:	nop    DWORD PTR [rax+0x0]    
```

B: `int b = bv`

```
0000000000011200 <main>:
1120a:  mov    ebx, DWORD PTR [rip+0x91e1c]    ; b = bv (читаем volatile в регистр)
11210:  mov    eax, 0x64                       ; a = 100
1121c:  cdq                                    ; расширение знака EAX → EDX:EAX
11222:  idiv   ebx                             ; ← деление, #DE при b == 0
11224:  mov    edx, eax                        ; r = результат
11228:  call   __printf_chk@plt                ; printf("[B] after division: r = %d\n", r)
1122d:  test   ebx, ebx                        ; ← ПРОВЕРКА b == 0
1122f:  jne    11241 <main+0x41>               ; если b != 0 → переход к "b is NOT 0"
11231:  lea    rdi, [rip+0x1cde8]              ; fmt_is0 = "b is 0"
11238:  call   puts@plt
1123d:  xor    eax, eax
1123f:  pop    rbx
11240:  ret
11241:  lea    rdi, [rip+0x1cde3]              ; fmt_not0 = "b is NOT 0"
11248:  call   puts@plt
1124d:  jmp    1123d <main+0x3d>
```

**Вывод:** gcc `-O3` в обоих сценариях сохраняет ветку `if (b == 0)`. Оптимизатор не выводит постусловие `b != 0` из операции деления, поэтому `test/jne` остаются в машинном коде.

### 1.3. clang `-O0`

A: `volatile int b`

```
00000000000111e0 <main>:
   111e0:	push   rbp
   111e1:	mov    rbp,rsp
   111e4:	sub    rsp,0x10
   111e8:	mov    DWORD PTR [rbp-0x4],0x0
   111ef:	call   11260 <install_sigfpe_handler>
   111f4:	mov    DWORD PTR [rbp-0x8],0x64
   111fb:	mov    eax,DWORD PTR [rbp-0x8]
   111fe:	mov    ecx,DWORD PTR [rip+0x9bbac]        # acdb0 <b>
   11204:	cdq    
   11205:	idiv   ecx
   11207:	mov    DWORD PTR [rbp-0xc],eax
   1120a:	mov    esi,DWORD PTR [rbp-0xc]
   1120d:	lea    rdi,[rip+0x24df0]        # 36004 <_IO_stdin_used+0x4>
   11214:	mov    al,0x0
   11216:	call   11050 <printf@plt>
   1121b:	mov    eax,DWORD PTR [rip+0x9bb8f]        # acdb0 <b>
   11221:	cmp    eax,0x0
   11224:	jne    1123d <main+0x5d>
   1122a:	lea    rdi,[rip+0x24def]        # 36020 <_IO_stdin_used+0x20>
   11231:	mov    al,0x0
   11233:	call   11050 <printf@plt>
   11238:	jmp    1124b <main+0x6b>
   1123d:	lea    rdi,[rip+0x24de8]        # 3602c <_IO_stdin_used+0x2c>
   11244:	mov    al,0x0
   11246:	call   11050 <printf@plt>
   1124b:	xor    eax,eax
   1124d:	add    rsp,0x10
   11251:	pop    rbp
   11252:	ret    
   11253:	nop    WORD PTR cs:[rax+rax*1+0x0]
   1125d:	nop    DWORD PTR [rax] 
```

B: `int b = bv`

```
00000000000111e0 <main>:
   111e0:	push   rbp
   111e1:	mov    rbp,rsp
   111e4:	sub    rsp,0x10
   111e8:	mov    DWORD PTR [rbp-0x4],0x0
   111ef:	call   11260 <install_sigfpe_handler>
   111f4:	mov    DWORD PTR [rbp-0x8],0x64
   111fb:	mov    eax,DWORD PTR [rip+0x9bbaf]        # acdb0 <bv>
   11201:	mov    DWORD PTR [rbp-0xc],eax
   11204:	mov    eax,DWORD PTR [rbp-0x8]
   11207:	cdq    
   11208:	idiv   DWORD PTR [rbp-0xc]
   1120b:	mov    DWORD PTR [rbp-0x10],eax
   1120e:	mov    esi,DWORD PTR [rbp-0x10]
   11211:	lea    rdi,[rip+0x24dec]        # 36004 <_IO_stdin_used+0x4>
   11218:	mov    al,0x0
   1121a:	call   11050 <printf@plt>
   1121f:	cmp    DWORD PTR [rbp-0xc],0x0
   11223:	jne    1123c <main+0x5c>
   11229:	lea    rdi,[rip+0x24df0]        # 36020 <_IO_stdin_used+0x20>
   11230:	mov    al,0x0
   11232:	call   11050 <printf@plt>
   11237:	jmp    1124a <main+0x6a>
   1123c:	lea    rdi,[rip+0x24de9]        # 3602c <_IO_stdin_used+0x2c>
   11243:	mov    al,0x0
   11245:	call   11050 <printf@plt>
   1124a:	xor    eax,eax
   1124c:	add    rsp,0x10
   11250:	pop    rbp
   11251:	ret    
   11252:	nop    WORD PTR cs:[rax+rax*1+0x0]
   1125c:	nop    DWORD PTR [rax+0x0]
```

**Вывод:** картина совпадает с gcc `-O0` - оптимизации отключены, ветка сгенерирована в обоих сценариях.

### 1.4. clang `-O3`

A: `volatile int b` (ветка жива)

```
000000000000e1f0 <main>:
    e1f0:	push   rax
    e1f1:	call   e240 <install_sigfpe_handler>
    e1f6:	mov    DWORD PTR [rsp+0x4],0x64
    e1fe:	mov    eax,DWORD PTR [rsp+0x4]
    e202:	cdq    
    e203:	idiv   DWORD PTR [rip+0x8be83]        # 9a08c <b>
    e209:	lea    rdi,[rip+0x16df4]        # 25004 <_IO_stdin_used+0x4>
    e210:	mov    esi,eax
    e212:	xor    eax,eax
    e214:	call   e060 <printf@plt>
    e219:	cmp    DWORD PTR [rip+0x8be6c],0x0        # 9a08c <b>
    e220:	lea    rax,[rip+0x16e08]        # 2502f <_IO_stdin_used+0x2f>
    e227:	lea    rdi,[rip+0x16df2]        # 25020 <_IO_stdin_used+0x20>
    e22e:	cmove  rdi,rax
    e232:	call   e030 <puts@plt>
    e237:	xor    eax,eax
    e239:	pop    rcx
    e23a:	ret    
    e23b:	nop    DWORD PTR [rax+rax*1+0x0]
```

B: `int b = bv` (ветка вырезана)

```
000000000000e1f0 <main>:
    e1f0:	push   rax
    e1f1:	call   e230 <install_sigfpe_handler>
    e1f6:	mov    eax,0x64
    e1fb:	xor    edx,edx
    e1fd:	idiv   DWORD PTR [rip+0x8be89]        # 9a08c <bv>
    e203:	lea    rdi,[rip+0x16dfa]        # 25004 <_IO_stdin_used+0x4>
    e20a:	mov    esi,eax
    e20c:	xor    eax,eax
    e20e:	call   e060 <printf@plt>
    e213:	lea    rdi,[rip+0x16e06]        # сразу "b is NOT 0"
    e21a:	call   e030 <puts@plt>          # БЕЗ cmp/test/je
    e21f:	xor    eax,eax
    e221:	pop    rcx
    e222:	ret    
    e223:	nop    WORD PTR cs:[rax+rax*1+0x0]
    e22d:	nop    DWORD PTR [rax]
```

**Вывод:** clang 18 `-O3` в сценарии A сохраняет ветку (через cmp + cmove), а в сценарии B полностью удаляет её из машинного кода. Инструкций cmp, test, je, jne, связанных с `b == 0`, не остаётся вовсе.



## 2. Итоговый отчёт Debug vs Release

| Компилятор / флаги | A: `volatile int b` | B: `int b = bv` |
|---|---|---|
| gcc -O0 | ветка есть: `test` + `jne` | ветка есть: `cmp` + `jne` |
| gcc -O3 | ветка есть: `cmp` + `jne` (volatile) | ветка есть: `test` + `jne` |
| clang -O0 | ветка есть: `test` + `jne` | ветка есть: `cmp` + `jne` |
| clang -O3 | ветка есть: `cmp` + `cmove` (volatile) | ветка вырезана полностью |

### 2.1. Обоснование

Деление целых в C и C++ требует `b != 0`. Нарушение этого требования является Undefined Behavior. Компилятор вправе предполагать, что UB не наступает ни при каком исполнении, и строить оптимизации на этом предположении.

a) `int b = bv;` - скопировали значение 
Из volatile-переменной `bv` прочитали `0` и положили в обычную локальную `b`. После этого момента `b` - обычная переменная. Слово volatile действовало только на `bv` - оно заставляло перечитать значение из памяти. Как только копия попала в `b`, никакой защиты больше нет. Компилятор может рассуждать о `b` как хочет.

b) `int r = a / b;` — компилятор делает вывод
В стандарте C и C++ написано: делить на ноль нельзя. Если `b == 0` - поведение программы не определено. Это значит, что компилятор имеет право считать, что деление на ноль не произойдёт. Он не проверяет, действительно ли `b` не ноль. Он просто предполагает, что программист не допустит ошибки. Это называется «Undefined Behavior».

c) Компилятор запоминает, что b != 0. 

d) Компилятор видит if (b == 0) и понимает, что это ложь. Соответственно ненужный код удаляется.  Остаётся только printf("b is NOT 0"). Заодно исчезают инструкции cmp, test, je.

**Volatile** спасает, так как перед сравнением `b == 0` компилятор вынужден перечитать `b` из памяти и сравнить с нулем в момент выполнения

---

## 3. Итоговые выводы

1. **Перехват `SIGFPE` работает** — все восемь сборок завершаются штатно, ARM-семантика (`r = 0`) воспроизведена. Ни одна не упала, включая сборки, где clang использует RIP-relative `idiv`.

2. **`volatile` защищает от UB-оптимизаций.** Сценарий A даёт `b is 0` на любом компиляторе и любом уровне оптимизации: `volatile` обязывает перечитать значение из памяти.

3. **Без `volatile` поведение зависит от компилятора.**.

4. **Перехват `SIGFPE` не спасает от логических аномалий.** Обработчик `SIGFPE` и оптимизатор работают в разное время и с разными представлениями программы:
Оптимизатор работает до запуска во время компиляции. Он смотрит на исходный код C и решает, какие ветки оставить, а какие выбросить.
Обработчик работает во время выполнения, когда процесс уже собран, и в нём остался только тот машинный код, который оптимизатор счёл нужным.
К моменту, когда срабатывает `SIGFPE`, ветка `if (b == 0)` уже физически отсутствует в бинарнике. Обработчик может обнулять регистры, продвигать RIP, эмулировать ARM-семантику — но он не может вернуть в программу код, которого там нет.

---

## Приложение

```bash
# 1. Скачать Zydis amalgamated (single-file)
wget https://github.com/zyantific/zydis/releases/download/v4.1.0/zydis-amalgamated.tar.gz
tar -xzf zydis-amalgamated.tar.gz
mkdir -p zydis && cp zydis-amalgamated/Zydis.c zydis-amalgamated/Zydis.h zydis/

# 2. Собрать всё
make clean && make all

# 3. Прогон
for b in test_a_O0 test_a_O3 test_b_O0 test_b_O3 \
         test_a_clang_O0 test_a_clang_O3 test_b_clang_O0 test_b_clang_O3; do
    echo "================ $b ================"
    ./$b
    echo "exit: $?"
done | tee run_results.txt

# 4. Дизассемблирование ключевых бинарников
{
  echo "===== test_b_clang_O3 (ветка вырезана) ====="
  objdump -d -M intel --no-show-raw-insn test_b_clang_O3 | sed -n '/<main>:/,/^$/p'
  echo
  echo "===== test_a_clang_O3 (ветка жива, volatile) ====="
  objdump -d -M intel --no-show-raw-insn test_a_clang_O3 | sed -n '/<main>:/,/^$/p'
  echo
  echo "===== test_b_O3 (gcc 9.4, ветка жива) ====="
  objdump -d -M intel --no-show-raw-insn test_b_O3 | sed -n '/<main>:/,/^$/p'
} | tee disasm_key.txt
```