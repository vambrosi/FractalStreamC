/* main.c - program entry point (see cli.c). */
#include "all.c"

#if __has_include("cli.c")
int main(int argc, char **argv) { return cli_main(argc, argv); }
#else
int main(int argc, char **argv) {
    (void)argc; (void)argv;
    if (!glfwInit()) { fprintf(stderr, "glfwInit failed\n"); return 1; }
    printf("glfw %s vulkan=%d\n", glfwGetVersionString(), glfwVulkanSupported());
    glfwTerminate();
    return 0;
}
#endif
