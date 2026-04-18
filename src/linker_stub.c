/* linker_stub.c — standalone entry point for neutron-ld */
int linker_main(int argc, char **argv);
int main(int argc, char **argv) { return linker_main(argc, argv); }
