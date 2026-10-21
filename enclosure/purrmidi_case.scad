// =====================================================================
// PurrMIDI — корпус v0.1 (WeAct STM32H743 Mini + модуль PCM5102A)
// Параметрический, печать FDM без поддержек.
//
// Выбор детали:   part = "base" | "lid" | "cap" | "assembly" | "open"
//   openscad -D 'part="base"' -o base.stl purrmidi_case.scad
//
// Система координат корпуса: начало в левом нижнем внешнем углу,
// вид сверху. Низ (Y=0) — торец с USB-C и microSD, верх (Y=max) —
// торец с разъёмом 3.5 мм.
//
// ВСЁ, ЧТО ПОМЕЧЕНО [ПРОВЕРИТЬ], снято с чертежа PCB_sizes.pdf на глаз
// или предположено — сверьте штангенциркулем перед печатью.
// =====================================================================

part = "assembly";
$fn  = 40;

// ---------------- Плата WeAct STM32H743 Mini ----------------
bw      = 40.64;   // ширина платы
bl      = 66.88;   // длина платы
pcb_t   = 1.2;     // толщина платы                                [ПРОВЕРИТЬ]
hole_dx = 35.51;   // расстояние между крепёжными отверстиями по X
hole_dy = 61.38;   // ... по Y
usb_x   = 26.5;    // центр USB-C от левого края платы             [ПРОВЕРИТЬ]
usb_zc  = 1.7;     // центр разъёма над поверхностью платы         [ПРОВЕРИТЬ]
sd_x    = 20.3;    // центр слота microSD от левого края           [ПРОВЕРИТЬ]
sd_on_front = true;// слот microSD с нижней стороны платы, у торца USB [ПРОВЕРИТЬ]
tft_x   = 20.3;    // центр экрана от левого края платы            [ПРОВЕРИТЬ]
tft_y   = 52.6;    // центр экрана от нижнего края платы           [ПРОВЕРИТЬ]
tft_h   = 2.5;     // высота стекла над платой                     [ПРОВЕРИТЬ]
k1_x    = 19.0;    // центр кнопки K1 от левого края платы         [ПРОВЕРИТЬ]
k1_y    = 63.4;    // центр кнопки K1 от нижнего края платы        [ПРОВЕРИТЬ]
k1_h    = 4.3;     // высота толкателя тактовой кнопки над платой  [ПРОВЕРИТЬ]
btn_pitch = 5.8;   // шаг NR – B0 – K1

// ---------------- Модуль PCM5102A ----------------
dw      = 17.5;    // ширина                                       [ПРОВЕРИТЬ]
dl      = 31.5;    // длина                                        [ПРОВЕРИТЬ]
dac_t   = 1.6;     // толщина платы
jack_zc = 3.2;     // ось гнезда над платой                        [ПРОВЕРИТЬ]
jack_d  = 7.4;     // отверстие под гнездо/штекер 3.5 мм (для угловых штекеров с широким корпусом увеличьте)

// ---------------- Корпус ----------------
wall    = 2.4;
floor_t = 3.0;
lid_t   = 1.6;
stand   = 5.0;     // высота стоек под платой (microSD снизу + хвосты пинов)
top_clr = 5.5;     // зазор от платы до крышки (если на плате запаяна
                   // штыревая гребёнка SWD — увеличьте до 9)
c       = 0.5;     // зазор вокруг платы
dac_gap = 0.3;     // зазор вокруг DAC
dac_stand = 2.5;   // высота рельс под DAC
div_t   = 1.6;     // перегородка между платой и DAC
corner_r = 3.0;

// Окно экрана (видимая область 0.96" 160x80 ≈ 21.7 x 10.8)
win_w = 25.0;
win_h = 12.5;
win_chamfer = 1.0;

// Колпачок K1
cap_hole = 5.6;
cap_head = 5.2;
cap_flange = 7.0;
cap_protr = 0.8;   // насколько выступает над крышкой
cap_play  = 0.3;

pinholes = true;   // отверстия 2 мм под NR и B0 (сброс / DFU)

// ---------------- Производные размеры ----------------
iw   = c + bw + c + div_t + (dw + 2*dac_gap) + 0.3;
il   = c + bl + c;
ow   = iw + 2*wall;
ol   = il + 2*wall;
bx0  = wall + c;           // начало платы в координатах корпуса
by0  = wall + c;
z_pcb_bot = floor_t + stand;
z_pcb_top = z_pcb_bot + pcb_t;
z_wall    = z_pcb_top + top_clr;   // верх стенок основания = низ крышки

dac_x0 = bx0 + bw + c + div_t;               // левый край отсека DAC
dac_x1 = dac_x0 + dw + 2*dac_gap;
dac_xc = (dac_x0 + dac_x1) / 2;
dac_z_top = floor_t + dac_stand + dac_t;

function bxy(p) = [bx0 + p[0], by0 + p[1]];
hx0 = (bw - hole_dx)/2;
hy0 = (bl - hole_dy)/2;
holes = [[hx0, hy0], [hx0+hole_dx, hy0], [hx0, hy0+hole_dy], [hx0+hole_dx, hy0+hole_dy]];

module rbox(w, l, h, r) {
    hull() for (x=[r, w-r], y=[r, l-r]) translate([x, y, 0]) cylinder(r=r, h=h);
}

// ===================== ОСНОВАНИЕ =====================
module base() {
    difference() {
        union() {
            difference() {
                rbox(ow, ol, z_wall, corner_r);
                translate([wall, wall, floor_t]) rbox(iw, il, z_wall, 1.2);
            }
            // стойки под плату
            for (h = holes) translate([bxy(h)[0], bxy(h)[1], 0])
                cylinder(d=6.4, h=z_pcb_bot);
            // перегородка (в нижней части открыта для проводов DAC -> плата)
            translate([dac_x0 - div_t, wall + 16, 0])
                cube([div_t, il - 16, floor_t + 8]);
            // рельсы под DAC
            for (x = [dac_x0, dac_x1 - 1.4])
                translate([x, wall + 0.0, 0]) cube([1.4, il, floor_t + dac_stand]);
            // упоры DAC со стороны проводов
            for (x = [dac_x0, dac_x1 - 3])
                translate([x, wall + il - dl - 2*dac_gap - 1.5, 0])
                    cube([3, 1.5, floor_t + dac_stand + dac_t + 2]);
        }
        // винты M3 снизу (потай под ISO 7380 / DIN 7985)
        for (h = holes) translate([bxy(h)[0], bxy(h)[1], 0]) {
            translate([0,0,-1]) cylinder(d=3.4, h=z_pcb_bot + 2);
            translate([0,0,-0.01]) cylinder(d=6.4, h=1.9);
        }
        // USB-C (торец Y=0)
        translate([bx0 + usb_x - 6.5, -1, z_pcb_top + usb_zc - 3.25])
            cube([13, wall + 2, 6.5]);
        // microSD (торец Y=0, под платой)
        if (sd_on_front)
            translate([bx0 + sd_x - 7, -1, floor_t + 2.4])
                cube([14, wall + 2, z_pcb_bot - floor_t - 2.4 + 0.2]);
        // 3.5 мм (торец Y=max)
        translate([dac_xc, ol - wall - 1, floor_t + dac_stand + dac_t + jack_zc])
            rotate([-90,0,0]) cylinder(d=jack_d, h=wall + 2);
    }
}

// ===================== КРЫШКА =====================
module lid() {
    tx = bxy([tft_x, tft_y])[0];
    ty = bxy([tft_x, tft_y])[1];
    difference() {
        union() {
            // плита (в сборке лежит на z_wall)
            translate([0,0,z_wall]) rbox(ow, ol, lid_t, corner_r);
            // юбка, входящая внутрь стенок основания
            translate([0,0,z_wall - 2]) difference() {
                translate([wall + 0.25, wall + 0.25, 0]) rbox(iw - 0.5, il - 0.5, 2.01, 1);
                translate([wall + 1.45, wall + 1.45, -1]) rbox(iw - 2.9, il - 2.9, 4, 1);
            }
            // стойки под винты (прижимают плату)
            for (h = holes) translate([bxy(h)[0], bxy(h)[1], z_pcb_top])
                cylinder(d=5.6, h=z_wall - z_pcb_top + 0.01);
            // прижимы DAC
            for (x = [dac_x0 + 0.2, dac_x1 - 1.4])
                translate([x, wall + il - dl/2 - 3, dac_z_top])
                    cube([1.2, 6, z_wall - dac_z_top + 0.01]);
        }
        // пилотные отверстия под M3 (самонарезающие), глубина 8 мм
        for (h = holes) translate([bxy(h)[0], bxy(h)[1], z_pcb_top - 0.5])
            cylinder(d=2.7, h=8.5);
        // окно экрана с фаской 45° сверху
        hull() {
            translate([tx - win_w/2, ty - win_h/2, z_wall - 0.01]) cube([win_w, win_h, 0.01]);
            translate([tx - win_w/2, ty - win_h/2, z_wall + lid_t - win_chamfer])
                cube([win_w, win_h, 0.01]);
        }
        hull() {
            translate([tx - win_w/2, ty - win_h/2, z_wall + lid_t - win_chamfer])
                cube([win_w, win_h, 0.01]);
            translate([tx - win_w/2 - win_chamfer, ty - win_h/2 - win_chamfer, z_wall + lid_t])
                cube([win_w + 2*win_chamfer, win_h + 2*win_chamfer, 0.01]);
        }
        // K1
        translate([bxy([k1_x, k1_y])[0], bxy([k1_x, k1_y])[1], z_wall - 1])
            cylinder(d=cap_hole, h=lid_t + 2);
        // NR и B0 (слева от K1)
        if (pinholes) for (i = [1, 2])
            translate([bxy([k1_x - i*btn_pitch, k1_y])[0], bxy([k1_x - i*btn_pitch, k1_y])[1], z_wall - 1])
                cylinder(d=2.0, h=lid_t + 2);
    }
}

// ===================== КОЛПАЧОК K1 =====================
// Модель в рабочем положении: z=0 — верх толкателя кнопки.
// Фланец лежит на толкателе под плитой крышки, головка торчит сквозь отверстие.
module cap() {
    z_sw   = z_pcb_top + k1_h;
    fl_t   = 0.8;
    stem_l = max(0, z_wall - z_sw - fl_t - cap_play);
    head_l = cap_play + lid_t + cap_protr;
    if (stem_l > 0) cylinder(d=2.8, h=stem_l + 0.01);
    translate([0,0,stem_l]) cylinder(d=cap_flange, h=fl_t);
    translate([0,0,stem_l + fl_t]) cylinder(d=cap_head, h=head_l);
}

// ===================== ВЫВОД =====================
if (part == "base") base();
else if (part == "lid") translate([ow, 0, z_wall + lid_t]) rotate([0, 180, 0]) lid();   // лицом вниз для печати
else if (part == "cap") rotate([180,0,0]) cap();                                      // головкой вниз
else {
    color("#3a3f47") base();
    if (part != "open") color("#7a8591", 0.55) lid();
    if (part != "open") color("#e0a030") translate([bxy([k1_x, k1_y])[0], bxy([k1_x, k1_y])[1], z_pcb_top + k1_h]) cap();
    // «призрак» платы для проверки посадки
    color("#1c7c54", 0.8) translate([bx0, by0, z_pcb_bot]) cube([bw, bl, pcb_t]);
    color("#6b2f8a", 0.8) translate([dac_x0 + dac_gap, wall + il - dl - dac_gap, floor_t + dac_stand]) cube([dw, dl, dac_t]);
}
