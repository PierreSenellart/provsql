# Copy pre-built documentation directories verbatim into _site/ after each
# Jekyll build, bypassing Jekyll's processing (which would strip _static/ etc.).
# Populated by `make website`; safe to be absent (skipped if not yet built).

Jekyll::Hooks.register :site, :post_write do |site|
  %w[docs doxygen-sql doxygen-c].each do |dir|
    src = File.join(site.source, dir)
    dst = File.join(site.dest, dir)
    next unless File.directory?(src)
    FileUtils.mkdir_p(dst)
    FileUtils.cp_r(Dir.glob("#{src}/*"), dst)
  end
end
