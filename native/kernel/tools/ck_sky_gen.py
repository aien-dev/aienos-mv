#!/usr/bin/env python3
"""ck_sky_gen.py -- makes native/kernel/ui/sky_data.c: the real sky Allen
opens on, as tables the kernel draws from with integer arithmetic only.

Inputs (downloaded by scripts/ck_sky_gen.sh, each checked against the
sha256 below; nothing is fetched while the kernel runs):
  - the Yale Bright Star Catalogue, 5th revised edition (Hoffleit and
    Warren 1991; CDS V/50): every star to about magnitude 6.5, the
    naked-eye limit, with J2000 position, proper motion, V magnitude and
    B-V colour;
  - "The Milky Way panorama", ESO/S. Brunier (eso0932a, CC BY 4.0): a
    photograph of the whole sky, 6000 x 3000, in galactic longitude and
    latitude (checked here against the catalogue's stars);
  - OpenNGC (Mattia Verga, CC BY-SA 4.0): galaxy positions, sizes and
    orientations;
  - the GWTC-1 sky map of GW150914 (LIGO/Virgo, LIGO-P1800381): where the
    first black-hole merger ever heard came from.

What it works out (floating point is fine here; the kernel gets integers):
  - the view: a gnomonic (pinhole camera) projection centred on GW150914's
    most probable position, north up, a set width of sky across the screen;
  - the stars in that view as unit vectors (the kernel projects them), with
    a brightness code (flux = 10^(-0.4 m)) and a colour from B-V through a
    real temperature (Ballesteros 2012) to the colour of a black body of
    that temperature (CIE 1931 observer, sRGB);
  - the photograph re-projected into the same view, its stars taken out
    (the catalogue draws those, sharp), as a coarse grid of light;
  - the galaxies that are in the view and bright enough to see with the
    eye, at their true size and angle.

Usage: ck_sky_gen.py --cache DIR --out sky_data.c [--ra DEG --dec DEG]
       [--fov DEG] [--roll DEG] [--mag LIMIT] [--grid N] [--report FILE]
"""
import argparse, csv, gzip, hashlib, math, os, struct, sys, time

from PIL import Image, ImageFilter

SOURCES = {
    'bsc5': ('catalog.gz', 'https://cdsarc.cds.unistra.fr/ftp/V/50/catalog.gz',
             '3dc44b1e90be8fbe5bcc7656032560f51275f985c7e3f783c9028e1838ec7bed'),
    'milkyway': ('eso0932a.tif', 'https://cdn.eso.org/images/original/eso0932a.tif',
                 '10f209ab83e1fd89e7fa1ed70277ffc6ed19c43549f04ec637c6806d98aff035'),
    'ngc': ('NGC.csv', 'https://raw.githubusercontent.com/mattiaverga/OpenNGC/'
            'be3cd6c5498806a2fbb2f7e0452ef802dbb02be0/database_files/NGC.csv',
            '54abea0b57b0a103831bfc7a8e3a99a23acfbf4bb581fcf3d8af539cbb826c84'),
    'ngc_addendum': ('addendum.csv', 'https://raw.githubusercontent.com/mattiaverga/OpenNGC/'
                     'be3cd6c5498806a2fbb2f7e0452ef802dbb02be0/database_files/addendum.csv',
                     'c93f983dda6780b76b7b2e9271a856a892627710148a982ee98101a54c642fdd'),
    'gw150914': ('GW150914_skymap.fits.gz', 'https://dcc.ligo.org/public/0157/P1800381/007/GW150914_skymap.fits.gz',
                 'a8a4bdeb46aade1389e594e466ee6372a401157188df636eae2bbb3eab2805f2'),
}

EPOCH = 2026.8  # the stars are moved along their proper motion to this year

# The screen shape the grid is made for: centre of the view at half the width
# and 40% of the height (where allen_ui.c puts the merger); rows cover a
# 16:10 screen so a 1280x800 one is covered as well as 3840x2160.
CX_FRAC, CY_FRAC = 0.5, 0.40
H_OVER_W_MAX = 10.0 / 16.0


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for b in iter(lambda: f.read(1 << 20), b''):
            h.update(b)
    return h.hexdigest()


def vec(ra, dec):
    r, d = math.radians(ra), math.radians(dec)
    return (math.cos(d) * math.cos(r), math.cos(d) * math.sin(r), math.sin(d))


def dot(a, b):
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def sep_deg(a, b):
    return math.degrees(math.acos(max(-1.0, min(1.0, dot(a, b)))))


# ---- the GW150914 sky map (HEALPix, nested order) ----

def fits_table(path):
    raw = gzip.open(path).read()

    def header(off):
        cards = {}
        while True:
            block = raw[off:off + 2880]
            off += 2880
            for i in range(36):
                c = block[i * 80:(i + 1) * 80].decode('ascii')
                if c.startswith('END'):
                    return cards, off
                if c[8:10] == '= ':
                    cards[c[:8].strip()] = c[10:].split('/')[0].strip().strip("'").strip()

    h0, off = header(0)
    h1, off = header(off)
    return raw, h1, off


def healpix_nest_ang(nside, ipix):
    jrll = [2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4]
    jpll = [1, 3, 5, 7, 0, 2, 4, 6, 1, 3, 5, 7]

    def compress(v):
        r = 0
        for b in range(16):
            r |= ((v >> (2 * b)) & 1) << b
        return r
    npface = nside * nside
    face, ipf = divmod(ipix, npface)
    ix, iy = compress(ipf), compress(ipf >> 1)
    nl4 = 4 * nside
    jr = jrll[face] * nside - ix - iy - 1
    if jr < nside:
        nr, z, kshift = jr, 1 - jr * jr / (3.0 * npface), 0
    elif jr > 3 * nside:
        nr = nl4 - jr
        z, kshift = nr * nr / (3.0 * npface) - 1, 0
    else:
        nr, z, kshift = nside, (2 * nside - jr) * 2.0 / (3 * nside), (jr - nside) & 1
    jp = (jpll[face] * nr + ix - iy + 1 + kshift) // 2
    if jp > nl4:
        jp -= nl4
    if jp < 1:
        jp += nl4
    phi = (jp - (kshift + 1) * 0.5) * (math.pi / 2 / nr)
    return math.degrees(phi) % 360.0, 90.0 - math.degrees(math.acos(z))


def gw_map(path):
    raw, h1, off = fits_table(path)
    nside, rows, width = int(h1['NSIDE']), int(h1['NAXIS2']), int(h1['NAXIS1'])
    assert h1.get('ORDERING', 'NESTED').startswith('NEST'), h1.get('ORDERING')
    prob = [struct.unpack_from('>d', raw, off + i * width)[0] for i in range(rows)]
    order = sorted(range(rows), key=lambda i: -prob[i])
    tot, cum, n90 = sum(prob), 0.0, 0
    for k, i in enumerate(order):
        cum += prob[i]
        if cum >= 0.9 * tot:
            n90 = k + 1
            break
    best = healpix_nest_ang(nside, order[0])
    area = 4 * math.pi / (12 * nside * nside) * (180 / math.pi) ** 2
    region = [healpix_nest_ang(nside, i) for i in order[:n90]]
    return {'nside': nside, 'max': best, 'area90': n90 * area, 'region90': region,
            'threshold90': prob[order[n90 - 1]], 'prob': prob}


# ---- the view ----

class View:
    def __init__(self, ra, dec, fov, roll):
        self.ra, self.dec, self.fov, self.roll = ra, dec, fov, roll
        a, d = math.radians(ra), math.radians(dec)
        self.d0 = vec(ra, dec)
        east = (-math.sin(a), math.cos(a), 0.0)
        north = (-math.sin(d) * math.cos(a), -math.sin(d) * math.sin(a), math.cos(d))
        west = tuple(-e for e in east)
        cr, sr = math.cos(math.radians(roll)), math.sin(math.radians(roll))
        # screen right and screen up; roll turns the view clockwise on screen
        right = tuple(cr * w + sr * n for w, n in zip(west, north))
        up = tuple(-sr * w + cr * n for w, n in zip(west, north))
        self.rows = (right, up, self.d0)  # rotation into (right, up, forward)
        self.t = math.tan(math.radians(fov) / 2)  # tangent-plane units across half the width

    def to_screen(self, v, w, h):
        """sky direction -> screen px (x right, y down), None if behind."""
        x, y, z = (dot(r, v) for r in self.rows)
        if z <= 1e-6:
            return None
        p = (w / 2) / self.t
        return (w * CX_FRAC + p * x / z, h * CY_FRAC - p * y / z)

    def from_frac(self, fx, fy):
        """screen offset from the view centre, in widths -> sky direction."""
        u, v = fx * 2 * self.t, -fy * 2 * self.t
        r, up, f = self.rows
        d = tuple(f[i] + u * r[i] + v * up[i] for i in range(3))
        n = math.sqrt(dot(d, d))
        return tuple(c / n for c in d)


# ---- equatorial (J2000) to galactic (Hipparcos Vol. 1, sect. 1.5.3) ----

GAL = ((-0.0548755604, -0.8734370902, -0.4838350155),
       (0.4941094279, -0.4448296300, 0.7469822445),
       (-0.8676661490, -0.1980763734, 0.4559837762))


def galactic(v):
    g = tuple(dot(r, v) for r in GAL)
    return math.degrees(math.atan2(g[1], g[0])) % 360.0, math.degrees(math.asin(max(-1, min(1, g[2]))))


# ---- colour: B-V -> temperature -> black body -> sRGB ----

def ballesteros(bv):
    """B-V -> effective temperature, K (Ballesteros 2012, EPL 97, 34008)."""
    return 4600.0 * (1.0 / (0.92 * bv + 1.7) + 1.0 / (0.92 * bv + 0.62))


def cie1931(lam):
    """CIE 1931 2-degree observer, the multi-lobe fit of Wyman, Sloan and
    Shirley 2013 (JCGT 2(2)); lam in nm."""
    def g(x, mu, s1, s2):
        s = s1 if x < mu else s2
        return math.exp(-0.5 * ((x - mu) / s) ** 2)
    xb = 1.056 * g(lam, 599.8, 37.9, 31.0) + 0.362 * g(lam, 442.0, 16.0, 26.7) - 0.065 * g(lam, 501.1, 20.4, 26.2)
    yb = 0.821 * g(lam, 568.8, 46.9, 40.5) + 0.286 * g(lam, 530.9, 16.3, 31.1)
    zb = 1.217 * g(lam, 437.0, 11.8, 36.0) + 0.681 * g(lam, 459.0, 26.0, 13.8)
    return xb, yb, zb


def blackbody_rgb(T):
    """linear sRGB of a black body at T kelvin, brightest channel 1."""
    X = Y = Z = 0.0
    for lam in range(380, 781, 5):
        l = lam * 1e-9
        b = 1.0 / (l ** 5 * (math.exp(0.014387769 / (l * T)) - 1.0))
        xb, yb, zb = cie1931(lam)
        X, Y, Z = X + b * xb, Y + b * yb, Z + b * zb
    r = 3.2404542 * X - 1.5371385 * Y - 0.4985314 * Z
    g = -0.9692660 * X + 1.8760108 * Y + 0.0415560 * Z
    bl = 0.0556434 * X - 0.2040259 * Y + 1.0572252 * Z
    r, g, bl = max(r, 0.0), max(g, 0.0), max(bl, 0.0)
    m = max(r, g, bl)
    return r / m, g / m, bl / m


def srgb_decode(v):
    v /= 255.0
    return v / 12.92 if v <= 0.04045 else ((v + 0.055) / 1.055) ** 2.4


def srgb_encode(l):
    l = max(0.0, min(1.0, l))
    v = l * 12.92 if l <= 0.0031308 else 1.055 * l ** (1 / 2.4) - 0.055
    return v * 255.0


# ---- the Yale Bright Star Catalogue ----

def bsc5(path):
    stars, nobv = [], 0
    for line in gzip.open(path, 'rt', encoding='latin-1'):
        line = line.rstrip('\n').ljust(197)
        if not line[75:77].strip() or not line[102:107].strip():
            continue  # the 14 entries with no position (novae, clusters taken out)
        hr = int(line[0:4])
        ra = (int(line[75:77]) + int(line[77:79]) / 60 + float(line[79:83]) / 3600) * 15
        dec = int(line[84:86]) + int(line[86:88]) / 60 + int(line[88:90]) / 3600
        if line[83] == '-':
            dec = -dec
        v = float(line[102:107])
        bv = line[109:114].strip()
        if bv:
            bv = float(bv)
        else:
            bv, nobv = None, nobv + 1
        pmra = float(line[148:154]) if line[148:154].strip() else 0.0  # arcsec/yr, along RA (times cos dec)
        pmde = float(line[154:160]) if line[154:160].strip() else 0.0
        dt = EPOCH - 2000.0
        dec2 = dec + pmde * dt / 3600
        ra2 = ra + pmra * dt / 3600 / max(1e-6, math.cos(math.radians(dec)))
        stars.append({'hr': hr, 'name': line[4:14].strip(), 'ra': ra2 % 360, 'dec': dec2, 'v': v, 'bv': bv})
    return stars, nobv


# ---- OpenNGC ----

def openngc(paths):
    out = []
    for p in paths:
        with open(p, newline='', encoding='utf-8') as f:
            for row in csv.DictReader(f, delimiter=';'):
                if row['Type'] != 'G' or not row['RA']:
                    continue
                mag = row['V-Mag'] or row['B-Mag']
                if not mag:
                    continue
                h, m, s = (float(x) for x in row['RA'].split(':'))
                sd = -1 if row['Dec'].startswith('-') else 1
                d, dm, ds = (abs(float(x)) for x in row['Dec'].split(':'))
                out.append({'name': row['Name'], 'common': row['Common names'], 'ra': (h + m / 60 + s / 3600) * 15,
                            'dec': sd * (d + dm / 60 + ds / 3600), 'vmag': float(mag),
                            'bv': (float(row['B-Mag']) - float(row['V-Mag'])) if row['B-Mag'] and row['V-Mag'] else None,
                            'maj': float(row['MajAx'] or 0), 'min': float(row['MinAx'] or row['MajAx'] or 0),
                            'pa': float(row['PosAng'] or 0)})
    return out


# ---- the photograph ----

class Panorama:
    """eso0932a: galactic plate carree, l = 0 in the middle, l rising to the
    left, b = +90 at the top. Checked by measure() against catalogue stars."""

    def __init__(self, path):
        self.im = Image.open(path).convert('RGB')
        self.w, self.h = self.im.size
        self.dx = self.dy = 0.0

    def xy(self, l, b):
        x = (180.0 - ((l + 180.0) % 360.0 - 180.0)) / 360.0 * self.w - 0.5
        y = (90.0 - b) / 180.0 * self.h - 0.5
        return x + self.dx, y + self.dy

    def measure(self, stars, report):
        """where the photograph's bright stars are against where the catalogue
        says: the mean offset is applied, the scatter reported."""
        lum = self.im.convert('L')
        px = lum.load()
        offs = []
        for s in stars:
            if s['v'] > 2.5:
                continue
            l, b = galactic(vec(s['ra'], s['dec']))
            x, y = self.xy(l, b)
            if abs(b) > 80 or not (8 < x < self.w - 8 and 8 < y < self.h - 8):
                continue
            sw = sx = sy = 0.0
            ix, iy = int(round(x)), int(round(y))
            base = min(px[ix + i, iy + j] for i in (-7, 7) for j in (-7, 7))
            for j in range(-5, 6):
                for i in range(-5, 6):
                    v = max(0, px[ix + i, iy + j] - base) ** 2
                    sw, sx, sy = sw + v, sx + v * (ix + i), sy + v * (iy + j)
            if sw > 0:
                offs.append((sx / sw - x, sy / sw - y, s['name'] or 'HR %d' % s['hr'], s['v']))
        n = len(offs)
        mx = sorted(o[0] for o in offs)[n // 2]
        my = sorted(o[1] for o in offs)[n // 2]
        good = [o for o in offs if abs(o[0] - mx) < 2.5 and abs(o[1] - my) < 2.5]
        rms = math.sqrt(sum((o[0] - mx) ** 2 + (o[1] - my) ** 2 for o in good) / max(1, len(good)))
        report.append('photograph registration: %d stars brighter than V 2.5 found; median offset (%.2f, %.2f) px '
                      '(1 px = %.3f deg); %d within 2.5 px, rms %.2f px' %
                      (n, mx, my, 360.0 / self.w, len(good), rms))
        self.dx, self.dy = mx, my
        return mx, my, rms, len(good), n

    def starless(self, report):
        """the photograph with its stars taken out: a grey opening (minimum then
        maximum over 7 px, about 0.4 deg), then a light blur. What is left is
        the light of the stars too faint and too many to see one by one, the
        glowing gas and the dark dust."""
        t0 = time.time()
        im = self.im.filter(ImageFilter.MinFilter(7)).filter(ImageFilter.MaxFilter(7))
        im = im.filter(ImageFilter.GaussianBlur(2.5))
        self.clean = im
        self.cpx = im.load()
        report.append('photograph without its stars: grey opening 7 px then blur 2.5 px (%.1f s)' % (time.time() - t0))

    def photometry(self, stars, report):
        """the photograph's own light of catalogue stars V 3.5 to 6.0 away from
        the Milky Way's crowded middle (|b| > 15 deg): the sum, in linear light,
        of what the star adds over the starless photograph within 4 px. Fits
        log F = gamma x log(10^(-0.4 V)) + c: gamma 1 is a photograph whose
        light is proportional to the sky's."""
        px = self.im.load()
        pts = []
        for s in stars:
            if not 3.5 <= s['v'] <= 6.0:
                continue
            l, b = galactic(vec(s['ra'], s['dec']))
            if abs(b) < 15 or abs(b) > 75:
                continue
            x, y = self.xy(l, b)
            ix, iy = int(round(x)), int(round(y))
            if not (6 < ix < self.w - 6 and 6 < iy < self.h - 6):
                continue
            f = 0.0
            for j in range(-4, 5):
                for i in range(-4, 5):
                    if i * i + j * j > 16:
                        continue
                    o, c = px[ix + i, iy + j], self.cpx[ix + i, iy + j]
                    f += sum(0.2126 * (srgb_decode(o[k]) - srgb_decode(c[k])) * (k == 0) +
                             0.7152 * (srgb_decode(o[k]) - srgb_decode(c[k])) * (k == 1) +
                             0.0722 * (srgb_decode(o[k]) - srgb_decode(c[k])) * (k == 2) for k in range(3))
            if f > 0:
                pts.append((-0.4 * s['v'], math.log10(f)))
        n = len(pts)
        mx = sum(p[0] for p in pts) / n
        my = sum(p[1] for p in pts) / n
        sxx = sum((p[0] - mx) ** 2 for p in pts)
        sxy = sum((p[0] - mx) * (p[1] - my) for p in pts)
        gamma = sxy / sxx
        c = my - gamma * mx
        res = math.sqrt(sum((p[1] - (gamma * p[0] + c)) ** 2 for p in pts) / n)
        report.append('photograph photometry: %d stars V 3.5..6.0, |b| > 15: light ~ (10^(-0.4 V))^%.3f, '
                      'a magnitude 0 star = %.4g linear x photo pixels; scatter %.2f mag' %
                      (n, gamma, 10 ** c, res / 0.4))
        self.gamma, self.c = gamma, c
        return gamma, 10 ** c

    def sample(self, l, b):
        x, y = self.xy(l, b)
        x0, y0 = math.floor(x), math.floor(y)
        fx, fy = x - x0, y - y0
        acc = [0.0, 0.0, 0.0]
        for (i, j, wgt) in ((0, 0, (1 - fx) * (1 - fy)), (1, 0, fx * (1 - fy)), (0, 1, (1 - fx) * fy), (1, 1, fx * fy)):
            xx = (x0 + i) % self.w
            yy = min(max(y0 + j, 0), self.h - 1)
            c = self.cpx[xx, yy]
            for k in range(3):
                acc[k] += wgt * srgb_decode(c[k])
        return acc


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--list-sources', action='store_true')
    ap.add_argument('--cache')
    ap.add_argument('--out')
    ap.add_argument('--report')
    ap.add_argument('--ra', type=float)   # default: GW150914's most probable point
    ap.add_argument('--dec', type=float)
    ap.add_argument('--fov', type=float, default=80.0)   # degrees of sky across the screen's width
    ap.add_argument('--roll', type=float, default=0.0)   # degrees; 0 is north up
    ap.add_argument('--mag', type=float, default=6.5)    # faintest star, V
    ap.add_argument('--grid', type=int, default=240)     # Milky Way grid columns across the width
    ap.add_argument('--boost', type=float, default=5.0)  # all diffuse light (Milky Way, sky glow) over the stars, times
    ap.add_argument('--skyglow', type=float, default=22.0)  # the dark sky's own light, V mag per square arcsec
    ap.add_argument('--mw-top-s10', type=float, default=900.0)  # the brightest Milky Way in the view, S10(V)
    ap.add_argument('--exposure', type=float, default=0.06)  # linear light at the centre of a magnitude 6.5 star
    a = ap.parse_args()
    if a.list_sources:
        for name, url, want in SOURCES.values():
            print(name, url, want)
        return
    if not a.cache or not a.out:
        ap.error('--cache and --out are needed')
    report = []
    t0 = time.time()
    files = {}
    for k, (name, url, want) in SOURCES.items():
        p = os.path.join(a.cache, name)
        got = sha256(p)
        if got != want:
            sys.exit('ck_sky_gen: %s has sha256 %s, expected %s (from %s)' % (p, got, want, url))
        files[k] = p
        report.append('input %s: %s sha256 %s' % (k, url, got))

    gw = gw_map(files['gw150914'])
    ra = a.ra if a.ra is not None else round(gw['max'][0], 3)
    dec = a.dec if a.dec is not None else round(gw['max'][1], 3)
    report.append('GW150914 sky map: HEALPix nside %d; most probable point RA %.3f Dec %.3f deg; 90%% region %.1f sq deg'
                  % (gw['nside'], gw['max'][0], gw['max'][1], gw['area90']))
    view = View(ra, dec, a.fov, a.roll)
    report.append('view: gnomonic, centre RA %.3f Dec %.3f deg, %.1f deg across the width, roll %.1f deg, '
                  'centre at %.0f%% across and %.0f%% down' % (ra, dec, a.fov, a.roll, CX_FRAC * 100, CY_FRAC * 100))
    W, H = 3840, 2160
    tl = view.from_frac(-0.5, -CY_FRAC * H / W)
    br = view.from_frac(0.5, (1 - CY_FRAC) * H / W)
    report.append('3840x2160: the top-left corner is %.1f deg from the centre, the bottom-right %.1f deg' %
                  (sep_deg(tl, view.d0), sep_deg(br, view.d0)))
    lc, bc = galactic(view.d0)
    report.append('the centre in galactic coordinates: l %.2f b %.2f deg' % (lc, bc))
    inside = sum(1 for (r_, d_) in gw['region90'] if view.to_screen(vec(r_, d_), W, H) and
                 0 <= view.to_screen(vec(r_, d_), W, H)[0] < W and 0 <= view.to_screen(vec(r_, d_), W, H)[1] < H)
    report.append('of the GW150914 90%% region, %.0f%% of its pixels are on the 3840x2160 screen' %
                  (100.0 * inside / len(gw['region90'])))

    # ---- stars ----
    stars, nobv = bsc5(files['bsc5'])
    report.append('catalogue: %d stars with positions; %d have no B-V (drawn at B-V 0.6, close to the Sun\'s)' %
                  (len(stars), nobv))
    margin = 64
    hmax = W * H_OVER_W_MAX

    def on(v, m=margin):
        p = view.to_screen(v, W, hmax / (1.0))
        if p is None:
            return None
        # rows: from -0.4 * (h/w max) to +0.6 * (h/w max) widths
        x, y = p
        y = y  # y measured with the 16:10 height
        return p if (-m <= x < W + m and -m <= y < hmax + m) else None

    def on_169(v):
        p = view.to_screen(v, W, H)
        return p is not None and 0 <= p[0] < W and 0 <= p[1] < H

    sel = []
    counts = {}
    for s in stars:
        v = vec(s['ra'], s['dec'])
        for lim in (4.0, 5.0, 5.5, 6.0, 6.5, 7.0, 99):
            if s['v'] <= lim and on_169(v):
                counts[lim] = counts.get(lim, 0) + 1
        if s['v'] <= a.mag and on(v):
            sel.append((s, v))
    report.append('stars on the 3840x2160 screen by limit: ' +
                  ', '.join('V<=%s: %d' % ('all' if k == 99 else k, counts.get(k, 0)) for k in sorted(counts)))
    allsky = sum(1 for s in stars if s['v'] <= a.mag)
    report.append('stars to V %.1f in the whole sky: %d (8 bytes each: %d bytes); in this view with its margin: %d' %
                  (a.mag, allsky, allsky * 8, len(sel)))
    sel.sort(key=lambda sv: sv[0]['v'])

    # colour palette: B-V from -0.40 to 2.14 in steps of 0.02
    pal = []
    for i in range(128):
        bv = -0.40 + 0.02 * i
        pal.append(blackbody_rgb(ballesteros(bv)))
    for bv in (-0.3, 0.0, 0.65, 1.5):
        T = ballesteros(bv)
        r, g, b = blackbody_rgb(T)
        report.append('colour: B-V %.2f -> %.0f K -> sRGB (%.0f, %.0f, %.0f)' %
                      (bv, T, srgb_encode(r), srgb_encode(g), srgb_encode(b)))

    # ---- the Milky Way ----
    pano = Panorama(files['milkyway'])
    report.append('photograph: %d x %d, %.3f deg per pixel' % (pano.w, pano.h, 360.0 / pano.w))
    pano.measure(stars, report)
    pano.starless(report)
    pano.photometry(stars, report)  # a diagnostic only: the photograph is not photometric (see the report)
    # The photograph's light is set from a measured value instead: the mean surface brightness of the
    # Milky Way over the whole sky, the stars fainter than 6.5 included, from Pioneer 10/11 photometry:
    # 101 S10(V) in B and 136 in R (Mattila et al. 2018, A&A 617, A42, table 5; from Toller 1981 and
    # Leinert et al. 1998); taken here as their mean, 118 S10(V). One S10 is one 10th-magnitude star per
    # square degree. The photograph's own mean over the sphere (weighted by cos b) is made equal to it.
    r0 = 1.2  # design px: the core of a star
    star0 = a.exposure * (math.pi * r0 * r0 / 4) / 0.8 * 10 ** (0.4 * 6.5)  # a magnitude 0 star's light, linear x design px
    px_deg2 = (a.fov / 1920.0) ** 2  # a design px at the centre, square degrees
    s10 = 1e-4 * star0 * px_deg2      # linear light per design px of 1 S10 (one 10th-magnitude star per square degree)
    a.glow_lin = 10 ** (0.4 * (27.78 - a.skyglow)) * s10 * a.boost
    report.append('display: a magnitude 6.5 star peaks at %.3f linear (sRGB level %.0f); a magnitude 0 star holds '
                  '%.1f linear x design px; 1 S10 is %.3g linear per design px' %
                  (a.exposure, srgb_encode(a.exposure), star0, s10))
    report.append("the dark sky's own glow, V %.1f mag per square arcsec (%.0f S10; airglow and zodiacal light at a dark "
                  "site): %.5f linear per design px (sRGB level %.1f)" %
                  (a.skyglow, 10 ** (0.4 * (27.78 - a.skyglow)), a.glow_lin, srgb_encode(a.glow_lin)))
    gw_cols = a.grid
    gw_rows = int(math.ceil(gw_cols * H_OVER_W_MAX)) + 2
    gy0 = -CY_FRAC * H_OVER_W_MAX  # widths
    step = 1.0 / gw_cols
    grid = []
    for j in range(gw_rows):
        for i in range(gw_cols + 1):
            d = view.from_frac(-0.5 + i * step, gy0 + j * step)
            l, b = galactic(d)
            c = pano.sample(l, b)
            grid.append(c)
    # the photograph's darkest sky (its 2nd percentile of light) is taken as
    # its black: what is under that is the photographer's floor, not the sky.
    lums = sorted(0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2] for c in grid)
    floor = lums[len(lums) // 50]
    top = lums[len(lums) * 999 // 1000]
    # The photograph is not photometric (the report's photometry line: its stars' light hardly follows
    # their magnitudes), so its light is tied to a measurement: the Bochum V photometry of the southern
    # Milky Way (Leinert et al. 1998, A&AS 127, 1, section 10, fig. 65: its scale tops out at 900 S10(V);
    # the Carina-Coalsack isophotes in V are 250, 500 and 800 S10). The view's brightest Milky Way (the
    # 99.9th percentile of the grid) is set to --mw-top-s10, the photograph's black to nothing.
    a.g_phys = a.mw_top_s10 * s10 / (top - floor)
    report.append('the photograph tied to Leinert et al. 1998: the brightest Milky Way in the view = %.0f S10(V), '
                  'linear x %.4f per design px; the dark sky\'s glow %.0f S10; all of that shown x %.2f against the '
                  'stars (--boost; 1 is the true contrast at one design pixel, 2.5 arcmin)' %
                  (a.mw_top_s10, a.g_phys, 10 ** (0.4 * (27.78 - a.skyglow)), a.boost))
    report.append('Milky Way grid: %d x %d samples, %.3f deg apart at the centre; photograph light (linear): '
                  '2nd percentile %.4f (taken as black), 99.9th %.4f' %
                  (gw_cols + 1, gw_rows, math.degrees(2 * view.t * step), floor, top))

    # galaxies in the view, bright enough for the eye
    gals = []
    for g in openngc([files['ngc'], files['ngc_addendum']]):
        v = vec(g['ra'], g['dec'])
        if g['vmag'] > a.mag or not on_169(v):
            continue
        gals.append((g, v))
    gals.sort(key=lambda gv: gv[0]['vmag'])
    report.append('galaxies on screen brighter than V %.1f (OpenNGC): %s' %
                  (a.mag, ', '.join('%s %s V %.2f %.0fx%.0f arcmin PA %.0f' %
                                    (g['name'], g['common'].split(',')[0], g['vmag'], g['maj'], g['min'], g['pa'])
                                    for g, _ in gals) or 'none'))

    emit(a, view, sel, pal, grid, gw_cols, gw_rows, gy0, step, floor, gals, gw, ra, dec, report, pano)
    report.append('made in %.1f s' % (time.time() - t0))
    text = '\n'.join(report) + '\n'
    sys.stdout.write(text)
    if a.report:
        open(a.report, 'w').write(text)


def emit(a, view, sel, pal, grid, gw_cols, gw_rows, gy0, step, floor, gals, gw, ra, dec, report, pano):
    Q30 = 1 << 30
    out = []
    w = out.append
    w('/* sky_data.c -- GENERATED by native/kernel/tools/ck_sky_gen.py; do not edit.')
    w(' * Regenerate: scripts/ck_sky_gen.sh (downloads the inputs, checks their sha256).')
    w(' *')
    w(' * Stars: Yale Bright Star Catalogue, 5th revised ed. (Hoffleit & Warren 1991),')
    w(' *   CDS catalogue V/50, positions moved by proper motion to %.1f.' % EPOCH)
    w(' * Milky Way: "The Milky Way panorama", ESO/S. Brunier (eso0932a), CC BY 4.0,')
    w(' *   re-projected, its stars taken out.')
    w(' * Galaxies: OpenNGC (Mattia Verga), CC BY-SA 4.0.')
    w(' * View centre: the most probable point of the GW150914 sky map, GWTC-1,')
    w(' *   LIGO/Virgo (LIGO-P1800381-v7).')
    w(' * Settings: centre RA %.3f Dec %.3f deg, %.1f deg across, roll %.1f deg, faintest V %.1f,' %
      (ra, dec, a.fov, a.roll, a.mag))
    w(' *   grid %d, exposure %.4f, sky glow %.1f mag/arcsec2, Milky Way boost %.2f. */' %
      (gw_cols, a.exposure, a.skyglow, a.boost))
    w('#include "sky_data.h"')
    w('')
    w('const ck_sky_view ck_sky_the_view = {')
    w('    .ra_mdeg = %d, .dec_mdeg = %d, .fov_mdeg = %d, .roll_mdeg = %d,' %
      (round(ra * 1000), round(dec * 1000), round(a.fov * 1000), round(a.roll * 1000)))
    w('    .tan_half_q30 = %d,' % round(view.t * Q30))
    w('    .rot_q30 = {')
    for r in view.rows:
        w('        {%s},' % ', '.join('%d' % round(c * Q30) for c in r))
    w('    },')
    w('};')
    w('')
    # stars
    w('/* x, y, z: the direction, Q15 (J2000 axes); mag: (V + 2) * 20; bv: palette index */')
    w('const ck_sky_star ck_sky_stars[%d] = {' % len(sel))
    for s, v in sel:
        bv = s['bv'] if s['bv'] is not None else 0.6
        bvi = max(0, min(127, int(round((bv + 0.40) / 0.02))))
        mc = max(0, min(255, int(round((s['v'] + 2.0) * 20))))
        w('    {%d, %d, %d, %d, %d}, /* HR %d %s V %.2f B-V %s */' %
          (round(v[0] * 32767), round(v[1] * 32767), round(v[2] * 32767), mc, bvi, s['hr'], s['name'],
           s['v'], '%.2f' % s['bv'] if s['bv'] is not None else 'none'))
    w('};')
    w('const unsigned ck_sky_nstars = %d;' % len(sel))
    w('')
    # brightness of a star at each magnitude code: linear light at the centre
    # of its core at the reference scale (k = 256, 1920 px wide), Q16
    w('/* light at the middle of a star of magnitude code m, linear, Q16: exposure x 10^(-0.4 (V - 6.5)) */')
    vals = []
    for m in range(256):
        V = m / 20.0 - 2.0
        vals.append(min(0x7fffffff, int(round(a.exposure * 10 ** (-0.4 * (V - 6.5)) * 65536))))
    w('const uint32_t ck_sky_flux[256] = {')
    for i in range(0, 256, 8):
        w('    ' + ', '.join('%d' % x for x in vals[i:i + 8]) + ',')
    w('};')
    w('')
    w('/* the colour of a black body at the temperature of B-V = -0.40 + 0.02 i (Ballesteros 2012),')
    w(' * linear sRGB, the brightest channel 4096 */')
    w('const uint16_t ck_sky_palette[128][3] = {')
    for i, (r, g, b) in enumerate(pal):
        w('    {%d, %d, %d}, /* B-V %.2f, %.0f K */' % (round(r * 4096), round(g * 4096), round(b * 4096),
                                                     -0.40 + 0.02 * i, ballesteros(-0.40 + 0.02 * i)))
    w('};')
    w('')
    # sRGB tables
    w('/* sRGB level -> linear light, Q15 */')
    dec_t = [round(srgb_decode(i) * 32768) for i in range(256)]
    w('const uint16_t ck_sky_lin[256] = {')
    for i in range(0, 256, 16):
        w('    ' + ', '.join('%d' % x for x in dec_t[i:i + 16]) + ',')
    w('};')
    w('/* linear light (Q15 >> 3, 0..4095) -> sRGB level x 16 (sixteenths, for dithering) */')
    enc = [min(4080, round(srgb_encode(i / 4095.0) * 16)) for i in range(4096)]
    w('const uint16_t ck_sky_srgb16[4096] = {')
    for i in range(0, 4096, 16):
        w('    ' + ', '.join('%d' % x for x in enc[i:i + 16]) + ',')
    w('};')
    w('')
    # the Milky Way grid: luma full, chroma at half, all in sixteenths-of-level space
    lv = []
    gr, gg, gb = blackbody_rgb(ballesteros(0.8))  # the dark sky's colour, B-V about 0.8 (Benn and Ellison 1998)
    gl = 0.2126 * gr + 0.7152 * gg + 0.0722 * gb
    glow = [a.glow_lin * x / gl for x in (gr, gg, gb)]
    for c in grid:
        # surface brightness is the same wherever it lands on the screen (radiance is conserved)
        cc = [glow[k] + max(0.0, c[k] - floor) * a.g_phys * a.boost for k in range(3)]
        lv.append([srgb_encode(x) for x in cc])  # sRGB levels 0..255 (float)
    cols = gw_cols + 1
    Y = bytearray()
    for c in lv:
        Y.append(max(0, min(255, int(round((c[0] + 2 * c[1] + c[2]) / 4)))))
    hc, hr = (cols + 1) // 2, (gw_rows + 1) // 2
    CO, CG = [], []
    for j in range(hr):
        for i in range(hc):
            acc_o = acc_g = n = 0
            for dj in (0, 1):
                for di in (0, 1):
                    ii, jj = min(cols - 1, 2 * i + di), min(gw_rows - 1, 2 * j + dj)
                    r, g, b = lv[jj * cols + ii]
                    acc_o += (r - b) / 2
                    acc_g += (-r + 2 * g - b) / 4
                    n += 1
            CO.append(max(-127, min(127, int(round(acc_o / n * 2)))))  # half-levels
            CG.append(max(-127, min(127, int(round(acc_g / n * 2)))))
    w('/* the Milky Way in the view: %d x %d samples, %s widths apart, the first row %.6f widths' %
      (cols, gw_rows, step, gy0))
    w(' * above the view centre and the first column half a width left of it; luma (R + 2G + B) / 4 in sRGB')
    w(' * levels; colour (Co, Cg) at half the spacing, in half levels */')
    w('const ck_sky_map ck_sky_milkyway = {')
    w('    .cols = %d, .rows = %d, .per_width = %d, .row0_q16 = %d,' % (cols, gw_rows, gw_cols, round(gy0 * 65536)))
    w('};')
    w('const uint8_t ck_sky_luma[%d] = {' % len(Y))
    for i in range(0, len(Y), 24):
        w('    ' + ', '.join('%d' % x for x in Y[i:i + 24]) + ',')
    w('};')
    w('const int8_t ck_sky_chroma[%d][2] = {' % len(CO))
    for i in range(0, len(CO), 12):
        w('    ' + ', '.join('{%d, %d}' % (CO[k], CG[k]) for k in range(i, min(len(CO), i + 12))) + ',')
    w('};')
    w('')
    # galaxies: centre direction, axes and angle on screen worked out here
    w('/* galaxies in the view bright enough to see: centre (Q15 direction), half axes in widths Q16,')
    w(' * long axis angle on screen (turns / 65536, from screen right towards down), tint (sRGB) */')
    w('const ck_sky_galaxy ck_sky_galaxies[%d] = {' % max(1, len(gals)))
    W = 3840.0
    for g, v in gals:
        # the long axis on screen: a step along the position angle (from north through east)
        p0 = view.to_screen(v, W, W)
        pa = math.radians(g['pa'])
        a_, d_ = math.radians(g['ra']), math.radians(g['dec'])
        east = (-math.sin(a_), math.cos(a_), 0.0)
        north = (-math.sin(d_) * math.cos(a_), -math.sin(d_) * math.sin(a_), math.cos(d_))
        half = math.radians(g['maj'] / 120.0)
        dirv = tuple(math.cos(half) * v[i] + math.sin(half) * (math.cos(pa) * north[i] + math.sin(pa) * east[i])
                     for i in range(3))
        halfm = math.radians(g['min'] / 120.0)
        dirm = tuple(math.cos(halfm) * v[i] + math.sin(halfm) * (-math.sin(pa) * north[i] + math.cos(pa) * east[i])
                     for i in range(3))
        p1 = view.to_screen(dirv, W, W)
        p2 = view.to_screen(dirm, W, W)
        ax, ay = p1[0] - p0[0], p1[1] - p0[1]
        amaj = math.hypot(ax, ay) / W
        amin = math.hypot(p2[0] - p0[0], p2[1] - p0[1]) / W
        ang = (math.atan2(ay, ax) / (2 * math.pi)) % 1.0
        bv = g['bv'] if g['bv'] is not None else 0.6
        r, gg, b = blackbody_rgb(ballesteros(bv))
        tint = (round(srgb_encode(r)), round(srgb_encode(gg)), round(srgb_encode(b)))
        w('    {{%d, %d, %d}, %d, %d, %d, 0x%02x%02x%02xu}, /* %s, %s: V %.2f, %.1f x %.1f arcmin, PA %.0f deg */' %
          (round(v[0] * 32767), round(v[1] * 32767), round(v[2] * 32767), round(amaj * 65536), round(amin * 65536),
           round(ang * 65536) % 65536, tint[0], tint[1], tint[2], g['name'], g['common'].split(',')[0], g['vmag'],
           g['maj'], g['min'], g['pa']))
        report.append('galaxy %s: on screen half axes %.1f x %.1f px at 3840 wide, long axis at %.1f deg from right' %
                      (g['name'], amaj * W, amin * W, ang * 360))
    if not gals:
        w('    {{0, 0, 0}, 0, 0, 0, 0u},')
    w('};')
    w('const unsigned ck_sky_ngalaxies = %d;' % len(gals))
    w('')
    # a check list for the host test: named stars and where the generator puts them at 3840x2160
    w('/* for the host test: where these stars land on a 3840 x 2160 screen, 16ths of a pixel */')
    checks = []
    for s, v in sel[:40]:
        p = view.to_screen(v, 3840, 2160)
        if p and 0 <= p[0] < 3840 and 0 <= p[1] < 2160 and len(checks) < 8:
            checks.append((sel.index((s, v)), p, s))
    w('const ck_sky_check ck_sky_checks[%d] = {' % len(checks))
    for i, p, s in checks:
        w('    {%d, %d, %d}, /* HR %d %s */' % (i, round(p[0] * 16), round(p[1] * 16), s['hr'], s['name']))
    w('};')
    w('const unsigned ck_sky_nchecks = %d;' % len(checks))
    open(a.out, 'w').write('\n'.join(out) + '\n')
    nbytes = len(sel) * 8 + 256 * 4 + 128 * 6 + 512 + 8192 + len(Y) + 2 * len(CO) + 32 * max(1, len(gals)) + 200
    report.append('tables: stars %d B, Milky Way luma %d B + colour %d B, other tables about %d B' %
                  (len(sel) * 8, len(Y), 2 * len(CO), 256 * 4 + 128 * 6 + 512 + 8192))


if __name__ == '__main__':
    main()
