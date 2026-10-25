// =====================================================================
//  ESP32-C3-LCDkit  –  desk wedge case ("mini radar")      v4
//  Board mounted rotated 90° counter-clockwise: USB-C at the TOP,
//  rotary knob right of the display, antenna to the left.
//  Closed front: display window, RGB LED window and the encoder shaft
//  hole only. Remove the kit's knob cap; your own knob (Ø20) goes on
//  the shaft from outside and sits in a well that reaches down around
//  the encoder body, so it can sit right on top of the encoder and grip
//  as much of the short (15 mm) shaft as possible.
//  A 90° up/down USB-C adapter (bend toward the back of the PCB) sits
//  above the board inside the case; the cable plug goes straight out
//  through a hole in the back wall.
//
//  Two printed parts, no screws, no glue:
//    shell : wedge body with bezel. Print FACE DOWN, no supports.
//    base  : bottom plate, snaps into the shell. Print flat, hooks up.
//
//  Board positions below are in Espressif's photo orientation
//  ("layout-front" image: module at the top, USB on the right edge);
//  x from the left edge, y from the top edge, mm. The case applies
//  the 90° rotation itself.
//  (photo) = measured from the official photos, ~±0.5 mm.
//  (assumed) = stack-up height, check with calipers.
// =====================================================================

/* [What to render] */
part = "assembly"; // [shell, base, knob, fit_test, assembly]
show_board = true;  // dummy board + adapter + plug in the assembly view

/* [Cable / adapter] */
// up/down 90° adapter, female port facing the back of the PCB
// (default envelope ~ Delock 65914: 26.3 x 14.15 x 14.78 mm)
// measured on the real adapter: 11 mm past the board edge, 13 wide,
// 12 mm behind the PCB back surface
ad_len   = 10.1;  // body length beyond the board's USB socket (11 past the PCB edge)
ad_wid   = 13;    // body width
ad_front = 3.5;   // body in front of the plug centre line (toward the screen)
ad_back  = 15.2;  // body behind the plug centre line (= 12 behind the PCB back)
ad_port  = 5.5;   // socket mouth -> cable plug centre (photo: ~7 mm above the board edge)
plug_len = 25;    // cable plug + strain relief (round, from photo)
plug_w   = 9;     // round plug, ~8.5 mm
plug_t   = 9;
ad_clr   = 1.0;   // extra room around adapter and plug
hole_clr = 0.75;  // back-wall hole clearance around the plug

/* [Stand] */
tilt_back = 30;   // screen leans back from vertical (deg)

/* [Board] */
pcb_w = 54;                 // Espressif PCB drawing (photo x)
pcb_h = 61;                 // Espressif PCB drawing (photo y)
pcb_t = 1.6;
disp_top_h  = 14.3;         // (assumed) PCB top -> display glass top
// measured on the real board (USB up: 19 from left / 27 from top)
disp_c      = [27, 19];     // (measured) display centre
disp_view_d = 33.4;         // bezel opening, active area is Ø32.4
knob_c      = [27, 51];     // (measured) encoder shaft (USB up: 51 from left / 27 from top)
sub_half    = 20.25;        // LCD sub-board half-size around the display centre (photo)
// encoder: original cap removed, your own knob goes on the shaft
shaft_top_h = 15;           // shaft tip above the PCB top (measured)
enc_body_h  = 7;            // (assumed) encoder body / bushing top above the PCB – MEASURE
shaft_d     = 6;            // EC11 shaft
holes  = [[3.5,3.5],[50.5,3.5],[3.5,57.5],[50.5,57.5]]; // (photo)
// screws: M2.5 x 8 (self-tapping or machine screw cut into the plastic)
post_d      = 5.0;          // post diameter (sits on the hole's copper ring)
pilot_d     = 2.1;          // pilot hole for M2.5 (use 1.6 for M2)
pilot_depth = 8;            // how deep the screw can go into the post
driver_holes = false;       // Ø5 holes in the back wall, in line with the two top screws
usb_y  = 37.4;              // (photo) USB-C centre on the photo's right edge
usb_out = 0.9;              // (photo) receptacle sticks out past the PCB edge
reset_xy = [51.5, 49.1];    // (photo)
reset_h  = 2.5;             // tact switch height
rgb_xy   = [51.2, 22.5];    // (photo) WS2812B-mini
ir_tx_xy = [1.9, 12.0];     // (photo)
ir_rx_xy = [1.5, 35.0];     // (photo)
antenna_overhang = 5.0;     // module sticks out past the photo's top edge

/* [Shell] */
t_face     = 1.6;   // bezel thickness
t_wall     = 2.4;
clear_left  = 6.5;  // PCB edge -> wall, antenna side
clear_right_min = 5.0; // PCB edge -> wall, knob side (widened to fit the edge chamfer)
edge_chamfer = 3;   // 45° chamfer on the front's top, left and right edges
bezel_gap  = 0.4;   // glass -> bezel underside
chin_min   = 12;    // face below the board (raised automatically if needed)
top_depth_min = 23; // depth of the top bevel (raised to fit the adapter)
plate_t    = 2;     // bottom plate
knob_dia   = 20;    // your knob
knob_gap   = 1.0;   // radial gap knob -> pocket wall
knob_h     = 22;    // knob height (sits 8.8 deep in the well, rest sticks out)
knob_lift  = 0.5;   // knob bottom above the encoder body
well_lift  = 1.5;   // well wall stops this far above the PCB (clears SMD parts)

/* [Knob] */
knob_bore  = "D";   // [D, round]  D = shaft with a flat, round = knurled / plain shaft
bore_d     = 6.15;  // 6 mm shaft + print clearance (round: try 6.0 for a press fit)
bore_flat  = 4.65;  // D shafts: flat to opposite side (nominal 4.5)
grip_n     = 24;    // number of grip flutes
grip_r     = 0.9;   // flute radius
grip_depth = 0.65;  // how far the flutes cut into the knob
knob_chamfer = 1.0; // top edge chamfer
symbol_size  = 14;  // aircraft wingspan on the top face
symbol_depth = 0.8; // engraving depth (fill with paint for contrast)

/* [Hidden] */
$fn = 72;
eps = 0.01;

// ------------------------------------------------------------ derived
alpha  = 90 - tilt_back;                 // face angle above the desk
sa = sin(alpha); ca = cos(alpha);
v_pcb  = t_face + bezel_gap + disp_top_h;// depth of PCB top behind face
v_pcb2 = v_pcb + pcb_t;                  // depth of PCB back
w_usb  = -(v_pcb - 1.6);                 // USB-C axis depth (face frame Z)
latch_drop = 2.3;                        // screw heads below the PCB back

// Adapter + plug envelopes, relative to the receptacle mouth centre:
// [x range], [u range (along the face, + = up)], [w range (+ = toward the screen)]
ad_x = [-ad_wid/2, ad_wid/2];
ad_u = [0, ad_len];
ad_w = [-ad_back, ad_front];
pl_x = [-plug_w/2, plug_w/2];
pl_u = [ad_port - plug_t/2, ad_port + plug_t/2];
pl_w = [-ad_back - plug_len, -ad_back];

// chin: PCB back corner and the screw heads must clear the base
chin = ceil(max(chin_min, (plate_t + 1.5 + (v_pcb2 + latch_drop)*ca)/sa));

// right side: keep a 1.5 mm flat land between the knob-well rim and the chamfer
clear_right = max(clear_right_min,
    knob_c[1] + knob_dia/2 + knob_gap + 1.0 + 1.5 + edge_chamfer - pcb_h - t_wall);
X0 = t_wall + clear_left;                // board left edge (face X)
W  = X0 + pcb_h + clear_right + t_wall;  // board is pcb_h wide after rotation
U0 = chin + pcb_w;                       // board top edge (face Y)
u_r = U0 + usb_out;                      // receptacle mouth (face Y)
L  = u_r + ad_len + ad_clr + 0.5 + t_wall;   // face length
// top bevel deep enough that the adapter's back-top corner clears the back wall
u_c = u_r + ad_len + ad_clr;  w_c = w_usb - ad_back - ad_clr;
top_depth = max(top_depth_min, ceil(u_c - w_c + 0.8 + t_wall*sqrt(2) - L));
K  = L + top_depth;
Kin = K - t_wall*sqrt(2);
u_back = K / (1 + tan(alpha));
y_back = u_back*ca + u_back*tan(alpha)*sa; // footprint depth
// knob well: open-bottom tube from the face down to just above the PCB,
// around the encoder body. The knob sits knob_lift above the encoder.
pocket_r  = knob_dia/2 + knob_gap;
knob_bot  = enc_body_h + knob_lift;              // knob bottom above the PCB
engage    = shaft_top_h - knob_bot;              // shaft the knob can grip
knob_sink = v_pcb - knob_bot;                    // knob bottom below the face
knob_out  = knob_h - knob_sink;                  // how far the knob sticks out

// photo coords -> face frame, with the 90° counter-clockwise rotation
function B(p) = [X0 + p[1], U0 - pcb_w + p[0]];
Xu = B([pcb_w, usb_y])[0];               // USB axis X

module desk() rotate([alpha,0,0]) children();

module profile_x(pts, x0, x1)   // extrude a (Y,Z) profile along X
    translate([x0,0,0]) rotate([90,0,90]) linear_extrude(x1 - x0) polygon(pts);

// face-frame box helper (port-relative ranges)
module pbox(xr, ur, wr, c=0)
    translate([Xu + xr[0] - c, u_r + ur[0] - c, w_usb + wr[0] - c])
        cube([xr[1] - xr[0] + 2*c, ur[1] - ur[0] + 2*c, wr[1] - wr[0] + 2*c]);

// --------------------------------------------------------- body solids
module outer_ff()
    profile_x([[-200,0],[L,0],[L,-top_depth],
               [L-200,-top_depth-200],[-200,-top_depth-200]], 0, W);

module inner_ff() {
    zc = (L - t_wall) - Kin;
    profile_x([[-200,-t_face],[L-t_wall,-t_face],[L-t_wall,zc],
               [L-t_wall-200,zc-200],[-200,zc-200]], t_wall, W - t_wall);
}

// ------------------------------------------------- front edge chamfers
// cut: removes x - w < c (left), (W - x) - w < c (right), (L - u) - w < c (top)
module edge_chamfer_cuts(c=edge_chamfer) {
    if (c > 0) {
        translate([0, L + 10, 0]) rotate([90,0,0]) linear_extrude(L + 300)
            polygon([[-1, -c - 1], [c + 1, 1], [-1, 1]]);
        translate([0, L + 10, 0]) rotate([90,0,0]) linear_extrude(L + 300)
            polygon([[W + 1, -c - 1], [W - c - 1, 1], [W + 1, 1]]);
        profile_x([[L + 1, -c - 1], [L - c - 1, 1], [L + 1, 1]], -1, W + 1);
    }
}
// inside fillets so the chamfered corners keep ~1.6 mm of wall
module edge_fillets(c=edge_chamfer) {
    f = max(0, c + 1.6*sqrt(2) - (t_wall + t_face)) + 0.3;   // fillet leg
    if (c > 0 && f > 0.3) {
        translate([0, L + 10, 0]) rotate([90,0,0]) linear_extrude(L + 300) {
            polygon([[t_wall - eps, -t_face + eps], [t_wall + f, -t_face + eps], [t_wall - eps, -t_face - f]]);
            polygon([[W - t_wall + eps, -t_face + eps], [W - t_wall - f, -t_face + eps],
                     [W - t_wall + eps, -t_face - f]]);
        }
        profile_x([[L - t_wall + eps, -t_face + eps], [L - t_wall - f, -t_face + eps],
                   [L - t_wall + eps, -t_face - f]], 0, W);
    }
}

// --------------------------------------------------------- screw posts
// The board is screwed onto four posts through its own mounting holes,
// from the back (base removed). Self-tapping into a pilot hole.
module screw_posts() {
    for (h = holes) {
        p = B(h);
        translate([p[0], p[1], -v_pcb]) cylinder(d=post_d, h=v_pcb - 0.8);
        // small fillet foot where the post meets the front panel
        translate([p[0], p[1], -t_face - 1.5]) cylinder(d1=post_d, d2=post_d + 3, h=1.5 + eps);
    }
}
module screw_pilots() {
    for (h = holes) {
        p = B(h);
        translate([p[0], p[1], -v_pcb - 1]) cylinder(d=pilot_d, h=pilot_depth + 1, $fn=24);
        translate([p[0], p[1], -v_pcb - eps]) cylinder(d1=pilot_d + 0.8, d2=pilot_d, h=0.4, $fn=24);
        // optional screwdriver access through the back wall (top screws only)
        if (driver_holes && h[0] > pcb_w/2)
            translate([p[0], p[1], -200]) cylinder(d=5, h=200 - v_pcb2 - 4, $fn=32);
    }
}

// ------------------------------------------------- knob well
// The knob overhangs the PCB edge, so beyond the edge the well gets a
// solid shelf at PCB level instead of opening into the case.
well_bot = -(v_pcb - well_lift);
module pocket_wall() {
    k = B(knob_c);
    difference() {
        union() {
            translate([k[0], k[1], well_bot]) cylinder(r=pocket_r + 1.6, h=-well_bot - 0.8);
            intersection() {         // shelf + wall beside the PCB edge
                translate([k[0], k[1], -(v_pcb2 + 1)]) cylinder(r=pocket_r + 1.6, h=v_pcb2 + 0.2);
                translate([X0 + pcb_h + 0.5, -100, -100]) cube([100, 400, 100]);
            }
        }
        // the LCD sub-board / display sit right next to the knob: on that
        // side the well wall is removed all the way up to the front panel
        sub_keepout();
    }
}
module sub_keepout(c=1.0) {
    bbox([disp_c[0] - sub_half - c, disp_c[1] - sub_half - c],
         [disp_c[0] + sub_half + c, disp_c[1] + sub_half + c],
         -(v_pcb2 + 2), v_pcb2 + 2 - t_face);
}
module knob_cuts() {
    k = B(knob_c);
    // starts 1 mm below the wall bottom so the open end is clean
    // (also avoids coincident faces that look closed in the F5 preview)
    translate([k[0], k[1], well_bot - 1]) cylinder(r=pocket_r, h=-well_bot + 2);
    chamfer(k, 2*pocket_r, 1.0);
}

module inside_solids_ff() {
    screw_posts();
    // RGB light pipe
    r = B(rgb_xy);
    translate([r[0], r[1], -(v_pcb - 2.1)]) cylinder(d=5.0, h=v_pcb - 2.1 - 0.8);
    pocket_wall();
    edge_fillets();
}

// ----------------------------------------------------------------- cuts
module hole_thru(p, d) translate([p[0], p[1], -t_face - 1]) cylinder(d=d, h=t_face + 2);
module chamfer(p, d, c=0.8) translate([p[0], p[1], -c]) cylinder(d1=d, d2=d + 2*c + 0.02, h=c + 0.01);
module slot(p0, p1, w=1.5) {       // photo coords, both ends
    hull() for (q = [B(p0), B(p1)])
        translate([q[0], q[1], -t_face - 1]) cylinder(d=w, h=t_face + 2, $fn=24);
}

module cuts_ff() {
    d = B(disp_c);  hole_thru(d, disp_view_d);  chamfer(d, disp_view_d);
    knob_cuts();
    screw_pilots();
    edge_chamfer_cuts();
    // RGB LED window (through the light-pipe tube)
    r = B(rgb_xy);   translate([r[0], r[1], -v_pcb]) cylinder(d=3.2, h=v_pcb + 1);
    // cable plug exits straight out of the back wall
    pbox(pl_x, pl_u, [pl_w[0] - 60, pl_w[1] - 1], hole_clr + ad_clr/2);
}

// ------------------------------------------------ base snap hooks (desk)
hook_y = [y_back*0.30, y_back*0.68];
hook_w = 8;
hz0 = plate_t + 5.0;   // pocket bottom (catch)
module hook_pockets() {
    for (y = hook_y, side = [0,1])
        translate([side == 0 ? t_wall - 1.1 : W - t_wall - 0.5, y - (hook_w + 0.8)/2, hz0])
            cube([1.6, hook_w + 0.8, 2.8]);
}
module base_hook(side) {
    xo = t_wall + 0.25;                // arm outer face (left side)
    pts = [[xo, plate_t - eps], [xo + 1.4, plate_t - eps], [xo + 1.4, hz0 + 2.6],
           [xo, hz0 + 2.6], [t_wall - 0.85, hz0 + 0.7], [t_wall - 0.85, hz0 + 0.1],
           [xo, hz0 + 0.1]];
    for (y = hook_y)
        translate([0, y + hook_w/2, 0])
            if (side == 0) hk(pts);
            else translate([W, 0, 0]) mirror([1,0,0]) hk(pts);
}
module hk(pts) rotate([90,0,0]) linear_extrude(hook_w) polygon(pts);

// --------------------------------------------------------------- parts
module shell_desk() {
    difference() {
        intersection() {
            desk() union() {
                difference() { outer_ff(); inner_ff(); }
                intersection() { outer_ff(); inside_solids_ff(); }
            }
            translate([-1, -300, plate_t]) cube([W + 2, 600, 300]);
        }
        desk() cuts_ff();
        hook_pockets();
        translate([W/2 - 7, y_back - 9, plate_t - 1]) cube([14, 12, 2.2]); // pry slot
    }
}

module base_desk() {
    intersection() {
        desk() difference() { outer_ff(); edge_chamfer_cuts(); }
        translate([-1, -300, 0]) cube([W + 2, 600, plate_t]);
    }
    base_hook(0);
    base_hook(1);
}

// rough stand-in for the kit, for checking clearances (photo coords)
module bbox(p0, p1, z0, h) {
    a = B(p0); b = B(p1);
    translate([min(a[0], b[0]), min(a[1], b[1]), z0])
        cube([abs(a[0] - b[0]), abs(a[1] - b[1]), h]);
}
module board_ff() {
    color("darkgreen") bbox([0, 0], [pcb_w, pcb_h], -v_pcb2, pcb_t);
    color("silver")   bbox([20.4, -antenna_overhang], [33.6, 11.6], -v_pcb, 2.4);
    color("dimgray")  bbox([disp_c[0] - sub_half, disp_c[1] - sub_half],
                           [disp_c[0] + sub_half, disp_c[1] + sub_half], -(v_pcb - 11), 1.6);
    color("black")    bbox([19.2, 13], [35, 38], -v_pcb, 5);   // speaker (photo)
    d = B(disp_c);
    color("black") translate([d[0], d[1], -(v_pcb - 12.6)]) cylinder(d=35.4, h=disp_top_h - 12.6);
    for (x = [11.4, 43.1]) color("black") bbox([x - 1.27, 15.7], [x + 1.27, 33.5], -v_pcb, 11);
    for (x = [11.4, 43.1]) color("gray") bbox([x - 1.27, 15.7], [x + 1.27, 33.5], -v_pcb2 - 2.5, 2.5); // solder tails
    k = B(knob_c);   // EC11 body + shaft (cap removed)
    color("silver") translate([k[0] - 6.2, k[1] - 6.6, -v_pcb]) cube([12.4, 13.2, enc_body_h]);
    color("silver") translate([k[0], k[1], -v_pcb]) cylinder(d=shaft_d, h=shaft_top_h);
    color("silver") bbox([46.6, usb_y - 4.5], [pcb_w + usb_out, usb_y + 4.5], -v_pcb, 3.2);
    color("black") bbox([49, 7.7], [54, 19.2], -v_pcb, 11.5);   // 2x4 header
    color("black") bbox([0, 15], [3, 24], -v_pcb, 11);          // IR jumper
    c = B([10.8, 56]);
    color("orange") translate([c[0], c[1], -v_pcb]) cylinder(d=6.3, h=7.7);
    color("white") bbox([50, 47.6], [53, 50.6], -v_pcb, reset_h);
}
module adapter_ff() {
    color("royalblue") pbox(ad_x, ad_u, ad_w);
    color("tomato") pbox(pl_x, pl_u, pl_w);
}

// ------------------------------------------------------------ knob
// Printed upright (bore on the bed). The aircraft's nose points to the
// side of the D-flat, so the knob always goes on the same way round.
module aircraft2d(span) {
    s = span / 14;
    half = [[0, 7], [0.6, 6.4], [1.0, 5.4], [1.0, 2.2], [6.8, -0.6], [6.8, -1.8],
            [1.0, -0.6], [1.0, -4.4], [3.4, -6.0], [3.4, -6.9], [0.6, -6.3], [0, -6.6]];
    scale(s) union() {
        polygon(half);
        mirror([1, 0]) polygon(half);
    }
}
module knob() {
    R = knob_dia/2;  H = knob_h;  c = knob_chamfer;
    bore_h = shaft_top_h - knob_bot + 0.5;          // shaft length inside the knob + 0.5
    g0 = knob_sink - 1;                             // flutes start 1 mm below the case front
    difference() {
        rotate_extrude($fn=96)
            polygon([[0, 0], [R - 0.4, 0], [R, 0.4], [R, H - c], [R - c, H], [0, H]]);
        // grip flutes
        for (i = [0 : grip_n - 1]) rotate(i*360/grip_n)
            translate([R + grip_r - grip_depth, 0, g0]) cylinder(r=grip_r, h=H, $fn=24);
        // shaft bore with a lead-in chamfer
        translate([0, 0, -eps]) linear_extrude(bore_h + eps) bore2d();
        translate([0, 0, -eps]) cylinder(d1=bore_d + 1.0, d2=bore_d, h=0.5 + eps);
        // aircraft symbol
        translate([0, 0, H - symbol_depth]) linear_extrude(symbol_depth + 1) aircraft2d(symbol_size);
    }
}
module bore2d() {
    if (knob_bore == "D")
        intersection() {
            circle(d=bore_d, $fn=48);
            translate([-bore_d/2, -bore_d/2]) square([bore_d, bore_flat]);
        }
    else circle(d=bore_d, $fn=48);
}

// print orientations
module shell_print() rotate([180,0,0]) rotate([-alpha,0,0]) shell_desk();

if (part == "shell") shell_print();
else if (part == "base") base_desk();
else if (part == "knob") knob();
else if (part == "fit_test")       // bezel only, board area only
    intersection() {
        shell_print();
        translate([-1, -(U0 + 4), -eps]) cube([W + 2, U0 + 4 - (chin - 4), t_face + 2*eps]);
    }
else if (part == "assembly") {
    color("SlateGray") shell_desk();
    color("DimGray") base_desk();
    if (show_board) desk() {
        board_ff(); adapter_ff();
        k = B(knob_c);   // the knob, sitting knob_lift above the encoder body
        color("FireBrick") translate([k[0], k[1], -knob_sink]) knob();
    }
}

echo(str("chin=", chin, "  top_depth=", top_depth, "  footprint W x D = ",
         W, " x ", round(y_back), "  face length=", L,
         "  height=", round(L*sa),
         "  shaft grip=", engage, "  knob sunk=", knob_sink, "  knob sticks out=", knob_out));
