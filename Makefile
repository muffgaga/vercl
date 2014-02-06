# waf-based build flow :)

all:
	@./waf configure
	@./waf build

clean:
	@./waf clean

distclean mrproper:
	@./waf distclean
