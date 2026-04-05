PDFLATEX = pdflatex
PDFTOPPM = pdftoppm
CONVERT  = convert
INKSCAPE = inkscape

.PHONY: all clean
.INTERMEDIATE: favicon_render.png

all: logo.pdf logo.svg logo.png favicon.ico

logo.pdf: logo.tex
	$(PDFLATEX) -interaction=nonstopmode logo.tex

logo.svg: logo.pdf
	$(INKSCAPE) --export-type=svg --export-filename=$@ $<

logo.png: logo.pdf
	$(INKSCAPE) --export-type=png --export-dpi=300 --export-filename=$@ $<

favicon.pdf: favicon.tex
	$(PDFLATEX) -interaction=nonstopmode favicon.tex

favicon_render.png: favicon.pdf
	$(PDFTOPPM) -r 600 favicon.pdf favicon_render
	$(CONVERT) favicon_render-1.ppm -background white -alpha remove $@
	rm -f favicon_render-1.ppm

favicon.ico: favicon_render.png
	$(CONVERT) favicon_render.png \
	  \( -clone 0 -resize  64x64  -gravity center -background white -extent  64x64  \) \
	  \( -clone 0 -resize  48x48  -gravity center -background white -extent  48x48  \) \
	  \( -clone 0 -resize  32x32  -gravity center -background white -extent  32x32  \) \
	  \( -clone 0 -resize  16x16  -gravity center -background white -extent  16x16  \) \
	  -delete 0 $@
	rm -f favicon_render.png

clean:
	rm -f logo.pdf logo.svg logo.png logo.aux logo.log \
	      favicon.pdf favicon.ico favicon_render.png favicon_render-1.ppm \
	      favicon.aux favicon.log
