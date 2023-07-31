<div align="center">
<a href="https://a4term.com/">
<img src="extras/a4_logo.svg" alt="a4 logo" width="100px">
</a>
</div>

## About The Project

[A4](https://a4term.com/) is a dynamic terminal window manager. One of eight
tiling layouts is dynamically applied to all visible terminals. The terminal
windows are grouped by tags making it easy to visually switch between contexts
of work. Color schemes are dynamically applied to each terminal window, based
on the text in its titlebar, helping the user to visually identify terminals
based on attributes such as hostname, userid, and any other text programmed to
display in the titlebar.

Four of the eight layouts divide the screen into two areas, one for a zoomed
window and the other for a stack of the remaining windows. These four layouts
can be adjusted during use to expand or shrink the number of windows in the
zoom area and the overall size of the zoom area. They are named for the
location of the zoom area: zoom\_left, zoom\_right, zoom\_top, and
zoom\_bottom. The other four layouts are fullscreen, grid, rows, and columns.

Each terminal window can be tagged with one or more tags. The user then selects
which tag or tags to view, and all windows with those tags are arranged in the
dynamic layout. The list of all tags is displayed on the left of the status
bar. Tag names are user defined with the default being the numerals 1 through
9.

Terminal windows can be minimized to a bar at the bottom of the screen. A
minimized terminal keeps its tags but stays out of the user's way until it is
unminimized again. Terminal windows can be set to read-only status in order to
prevent accidental typing or closing. Terminal windows can be added to a focus
group in order to type the same input into all simultaneously. Minimized and
read-only terminal windows are never typed into, even if they are included in
the focus group.

Terminal color schemes are dynamically applied by comparing the text of the
terminal's title with an ordered set of user-defined color rules.

A4 supports 24-bit truecolor and is configurable by editing an a4.ini file.
It is a partial rewrite of
[dvtm](https://www.brain-dump.org/projects/dvtm/),
which in turn is a text-based implementation of
[dwm](https://dwm.suckless.org/).

![parts of the screen](extras/partsofscreen.png)

## Distribution Packages

#### [Void Linux]

Install using the xbps package manager

```sh
sudo xbps-install -S a4
```

#### [Nix]

Install using `nix-env`

``` sh
nix-env -iA nixos.a4term # change `nixos` for `nixpkgs`, if on a non-NixOS system
```

Try it with `nix-shell`

``` sh
nix-shell -p a4term
```

No other distribution packages at this time.

## a4 Compile and Install

tgz packages are available from the [Tags] page.

```sh
git clone https://github.com/rpmohn/a4
cd a4
make && sudo make install
```

### Prerequisites

#### libtickit

Requires [libtickit](https://www.leonerd.org.uk/code/libtickit/)
with a minimum version of
[0.4.3](https://www.leonerd.org.uk/code/libtickit/libtickit-0.4.3.tar.gz).
If you're compiling this yourself, make sure you also have installed both
[libtermkey-dev](https://www.leonerd.org.uk/code/libtermkey/)
(probably in your package manager but maybe not with required minimum
v0.22) and
[libunibilium-dev](https://github.com/mauke/unibilium)
(probably in your package manager).

```sh
curl https://www.leonerd.org.uk/code/libtickit/libtickit-0.4.3.tar.gz | tar xzf -
cd libtickit-0.4.3
make && sudo make install && sudo ldconfig
```

#### libvterm

Requires [libvterm](https://www.leonerd.org.uk/code/libvterm/)
with a minimum version of
[0.3.1](https://www.leonerd.org.uk/code/libvterm/libvterm-0.3.1.tar.gz).
```sh
curl https://www.leonerd.org.uk/code/libvterm/libvterm-0.3.1.tar.gz | tar xzf -
cd libvterm-0.3.1
make && sudo make install && sudo ldconfig
```

## Notes

### Persistence

It is useful to run a4 with 
[abduco](https://www.brain-dump.org/projects/abduco/) 
so that you can disconnect and reconnect while your a4 session 
continues to run in the background. This is also helpful if you run a4 
on remote machines since the session continues to run even if your 
connection to the machine is lost, and you can reconnect later without 
losing any of your work. Consider setting a alias such as this:
```sh
alias a4.abduco="abduco -A a4 a4 $@"
```

### Mouse Support

There's a configuration error in the xterm-256color file installed 
by some Linux distros that causes the mouse to behave incorrectly by 
printing characters to the terminal. If you experience this problem, 
run the following command to put a local, patched copy of the file in 
place for your login account:
```sh
infocmp xterm-256color | sed -E 's/(kmous=\\E\[)</\1M/' | tic -o ~/.terminfo -
```

## Documentation

See [a4term.com](https://a4term.com/) for more documentation, including a copy
of the manual page and default keyboard maps.

[Tags]: https://github.com/rpmohn/a4/tags
[Void Linux]: https://voidlinux.org/packages/?arch=x86_64&q=a4
[Nix]: https://search.nixos.org/packages?channel=unstable&from=0&size=50&sort=relevance&type=packages&query=a4term
