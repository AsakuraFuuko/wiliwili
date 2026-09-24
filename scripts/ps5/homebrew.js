async function main() {
    const CWD = window.workingDir;
    return {
        mainText: "wiliwili",
        secondaryText: "Bilibili client",
        imgPath: "/fs" + CWD + "/sce_sys/icon0.png",
        onclick: async () => ({
            path: CWD + "/wiliwili.elf",
            cwd: CWD
        })
    };
}
