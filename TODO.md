## Что осталось исправить
Дисплей рисуется блокирующим `HAL_SPI_Transmit` без DMA: поле 160×32 при SPI 15 МГц занимает около 5,5 мс.

`USB_PortLostRecover` вызывает `HAL_Delay(300)`. Это добавляет джиттер приёма MIDI. Рекомендация: SPI с DMA.

Удалить излишнюю диагностику, выводимую в USART:
[AUDIO] и [SF2], [USBEP] должны выводится только когда есть ошибки/переполнения/задержки

Убрать сообщение "[PC] ch=0 program 127 repeat ignored", просто игнорировать без сообщения

Перенести пользовательский код из /Core в другие каталоги соответственно назначению

Разобраться с картой памяти, сейчас вот так в сборке Release:
[build] Memory region         Used Size  Region Size  %age Used
[build]          DTCMRAM:       58520 B       128 KB     44.65%
[build]              RAM:        464 KB       512 KB     90.63%
[build]           RAM_D2:         17 KB       288 KB      5.90%
[build]           RAM_D3:           0 B        64 KB      0.00%
[build]          ITCMRAM:        4184 B        64 KB      6.38%
[build]            FLASH:      496540 B         2 MB     23.68%
Более 90% использования RAM потенциально опасно.

В синтезаторе Soundfont на сложных аккордах бывают хрипы и пропуски:
[AUDIO] soundfont: fill last=59 us max=3400 us (127% of 2666 us) >80%: 1086 >100%: 839 shed=844 late=848 calls=182547
[SF2] 1s: render avg=1946 max=2922 us (over budget=117 of 241 blocks) | voices max=28 now=0 | notes=0 regions want=0 started=0 short=0 (max/note=2) | mix peak=0.43 FS, over 1.0 FS: 0 samples in 0 blocks (soft limiter ON) | fifo dropped=0

Синтезатор dulcimer вместо звука выдаёт щелчок:
[AUDIO] dulcimer: fill last=3665 us max=4877 us (182% of 2666 us) >80%: 376 >100%: 161 shed=1571 late=1580 calls=242206

Синтезаторы flute и saxophone звучат очень плохо. Их нужно полностью переписать, желательно используя готовые библиотеки. 

Снабдить код, в тех местах где нету, комментариями на русском языке.

Если инициализацию какого-либо "железа" можно перенести из пользовательского кода в CubeMX, то сделать это (отредактировав PurrMidi.ioc и перегенерировав код в CubeMX).

# Улучшения

Перенести движок soundfont в конец списка.
Если в загруженном SF2 несколько инструментов/пресетов, то добавлять их все в список. На дисплее писать, например "SF:Piano".
