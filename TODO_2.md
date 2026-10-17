Удалить излишнюю диагностику, выводимую в USART:
[AUDIO] и [SF2], [USBEP] должны выводится только когда есть ошибки/переполнения/задержки

Убрать сообщение "[PC] ch=0 program 127 repeat ignored", просто игнорировать без сообщения

Разобраться с картой памяти, сейчас вот так в сборке Release:
Memory region         Used Size  Region Size  %age Used
          DTCMRAM:       57944 B       128 KB     44.21%
              RAM:        464 KB       512 KB     90.63%
           RAM_D2:         17 KB       288 KB      5.90%
           RAM_D3:           0 B        64 KB      0.00%
          ITCMRAM:        4184 B        64 KB      6.38%
            FLASH:      493768 B         2 MB     23.54%
Более 90% использования RAM потенциально опасно.

Перенести пользовательский код из /Core в другие каталоги соответственно назначению

В синтезаторе Soundfont на сложных аккордах бывают хрипы и пропуски:
[AUDIO] soundfont: fill last=59 us max=3400 us (127% of 2666 us) >80%: 1086 >100%: 839 shed=844 late=848 calls=182547
[SF2] 1s: render avg=1946 max=2922 us (over budget=117 of 241 blocks) | voices max=28 now=0 | notes=0 regions want=0 started=0 short=0 (max/note=2) | mix peak=0.43 FS, over 1.0 FS: 0 samples in 0 blocks (soft limiter ON) | fifo dropped=0

Синтезатор dulcimer вместо звука выдаёт щелчок:
[AUDIO] dulcimer: fill last=3665 us max=4877 us (182% of 2666 us) >80%: 376 >100%: 161 shed=1571 late=1580 calls=242206

Синтезаторы flute и saxophone звучат очень плохо. Их нужно полностью переписать, желательно используя готовые библиотеки. 

Снабдить код, в тех местах где нету, комментариями на русском языке.