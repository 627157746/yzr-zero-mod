// 影之刃零资源导出工具：读取游戏 IoStore 容器（AES + usmap），列出文件、导出属性 JSON、导出模型/贴图。
// 所有路径来自环境变量（由 tools\env.ps1 设置），缺任何一个直接报错，不猜默认值。
using System.Text;
using CUE4Parse.Compression;
using CUE4Parse.Encryption.Aes;
using CUE4Parse.FileProvider;
using CUE4Parse.UE4.Assets.Exports.Animation;
using CUE4Parse.UE4.Objects.Core.Math;
using CUE4Parse_Conversion.Animations;
using CUE4Parse.MappingsProvider.Usmap;
using CUE4Parse.UE4.Objects.Core.Misc;
using CUE4Parse.UE4.Versions;
using CUE4Parse.UE4.VirtualFileSystem;
using CUE4Parse_Conversion;
using CUE4Parse_Conversion.Options;
using Newtonsoft.Json;
using Serilog;

Console.OutputEncoding = Encoding.UTF8;
// CUE4Parse 内部用 Serilog 报告解析问题，只显示警告以上
Log.Logger = new LoggerConfiguration().MinimumLevel.Warning().WriteTo.Console().CreateLogger();

const string Usage = """
    PbzExport <command> [args]
      info                                     mount status and container list
      list   <pathPrefix> [--limit N]          list files by path prefix
      find   <text> [--limit N]                find files whose path contains text
      where  <path>                            show which container provides a file
      json   <packagePath> --out <file>        dump all exports of a package as JSON
      export <packagePath> --out <dir> [--mesh psk|gltf|ueformat] [--materials] [--sockets bone|socket|none]
      anim   <packagePath> --out <file> [--count N]   bone local transforms (UE space) of N evenly spaced frames
    global option: --with <dir>   also mount containers in <dir> (e.g. a mod's ~mods folder)
    """;

if (args.Length == 0)
{
    Console.WriteLine(Usage);
    return 1;
}

try
{
    using var provider = CreateProvider(Opt(args, "--with"));
    return args[0] switch
    {
        "info" => Info(provider),
        "list" => List(provider, Arg(args, 1, "pathPrefix"), Limit(args), prefix: true),
        "find" => List(provider, Arg(args, 1, "text"), Limit(args), prefix: false),
        "where" => Where(provider, Arg(args, 1, "path")),
        "json" => Json(provider, Arg(args, 1, "packagePath"), Opt(args, "--out") ?? throw new ArgumentException("missing --out <file>")),
        "export" => await Export(provider, Arg(args, 1, "packagePath"),
            Opt(args, "--out") ?? throw new ArgumentException("missing --out <dir>"),
            Opt(args, "--mesh") ?? "psk", args.Contains("--materials"), Opt(args, "--sockets") ?? "bone"),
        "anim" => Anim(provider, Arg(args, 1, "packagePath"), Opt(args, "--out") ?? throw new ArgumentException("missing --out <file>"),
            Opt(args, "--count") is { } c ? int.Parse(c) : 4),
        _ => throw new ArgumentException($"unknown command '{args[0]}'\n{Usage}")
    };
}
catch (Exception e)
{
    Console.Error.WriteLine($"ERROR: {e}");
    return 1;
}

static DefaultFileProvider CreateProvider(string? extraDir)
{
    // 解压库只用本地文件；文件不存在时 CUE4Parse 会自动联网下载，这里提前报错，避免静默下载
    var natives = Env("PBZ_NATIVES");
    var oodle = Path.Combine(natives, "oodle-data-shared.dll");
    var zlib = Path.Combine(natives, "zlib-ng2.dll");
    foreach (var dll in new[] { oodle, zlib })
        if (!File.Exists(dll)) throw new FileNotFoundException($"native library not found: {dll}");
    OodleHelper.Initialize(oodle);
    ZlibHelper.Initialize(zlib);

    // 只扫 Paks 顶层：不读 ~mods 里的 mod，保证导出的是原版资源；--with 时额外挂上指定目录（检查 mod 成品用）
    var extras = extraDir is null ? Array.Empty<DirectoryInfo>() : new[] { new DirectoryInfo(extraDir) };
    var provider = new DefaultFileProvider(new DirectoryInfo(Env("PBZ_PAKS")), extras, SearchOption.TopDirectoryOnly,
        new VersionContainer(EGame.GAME_PhantomBladeZero), StringComparer.OrdinalIgnoreCase);
    provider.Initialize();

    // aes.txt 里第一行非注释内容是主密钥（GUID 全 0）
    var key = File.ReadLines(Env("PBZ_AES")).Select(l => l.Trim()).First(l => l.Length > 0 && !l.StartsWith('#'));
    provider.SubmitKey(new FGuid(), new FAesKey(key));
    provider.PostMount();
    provider.MappingsContainer = new FileUsmapTypeMappingsProvider(Env("PBZ_USMAP"));
    return provider;
}

static int Info(DefaultFileProvider p)
{
    Console.WriteLine($"Project: {p.ProjectName}");
    Console.WriteLine($"Mounted: {p.MountedVfs.Count}  Unloaded: {p.UnloadedVfs.Count}  MissingKeys: {p.RequiredKeys.Count}  Files: {p.Files.Count}");
    foreach (var v in p.MountedVfs.OrderBy(v => v.Name))
        Console.WriteLine($"  [mounted]  {v.Name}  encrypted={v.IsEncrypted} encryptedFiles={v.EncryptedFileCount}");
    foreach (var v in p.UnloadedVfs.OrderBy(v => v.Name))
        Console.WriteLine($"  [UNLOADED] {v.Name}  encrypted={v.IsEncrypted} keyGuid={v.EncryptionKeyGuid}");
    return 0;
}

static int List(DefaultFileProvider p, string text, int limit, bool prefix)
{
    var hits = p.Files.Keys
        .Where(k => prefix ? k.StartsWith(text, StringComparison.OrdinalIgnoreCase) : k.Contains(text, StringComparison.OrdinalIgnoreCase))
        .Order(StringComparer.OrdinalIgnoreCase)
        .ToList();
    foreach (var k in hits.Take(limit)) Console.WriteLine(k);
    Console.WriteLine($"-- {hits.Count} match(es){(hits.Count > limit ? $", showing first {limit}" : "")}");
    return hits.Count > 0 ? 0 : 3;
}

static int Where(DefaultFileProvider p, string path)
{
    var key = Resolve(p, path);
    var f = p.Files[key];
    Console.WriteLine($"{key}  <-  {(f is VfsEntry v ? v.Vfs.Name : f.GetType().Name)}");
    return 0;
}

static int Json(DefaultFileProvider p, string packagePath, string outFile)
{
    var exports = p.LoadPackage(Resolve(p, packagePath)).GetExports().ToList();
    var full = Path.GetFullPath(outFile);
    Directory.CreateDirectory(Path.GetDirectoryName(full)!);
    File.WriteAllText(full, JsonConvert.SerializeObject(exports, Formatting.Indented));
    Console.WriteLine($"{exports.Count} export(s) -> {full}");
    foreach (var e in exports) Console.WriteLine($"  {e.ExportType}  {e.Name}");
    return 0;
}

static async Task<int> Export(DefaultFileProvider p, string packagePath, string outDir, string mesh, bool materials, string sockets)
{
    var format = mesh switch
    {
        "psk" => EMeshFormat.ActorX,
        "gltf" => EMeshFormat.Gltf2,
        "ueformat" => EMeshFormat.UEFormat,
        _ => throw new ArgumentException($"unknown --mesh '{mesh}' (psk|gltf|ueformat)")
    };
    var session = new ExportSession();
    foreach (var obj in p.LoadPackage(Resolve(p, packagePath)).GetExports())
    {
        // 包里的辅助子对象（LOD 设置、AssetUserData 等）没有导出器，跳过
        try { session.Add(obj); }
        catch (NotSupportedException) { Console.WriteLine($"  skip  {obj.ExportType}  {obj.Name}"); }
    }
    // 默认把插槽当成骨骼写进 psk；做空身体时用 none，只保留真实骨骼
    var socketFormat = sockets switch
    {
        "bone" => ESocketFormat.Bone,
        "socket" => ESocketFormat.Socket,
        "none" => ESocketFormat.None,
        _ => throw new ArgumentException($"unknown --sockets '{sockets}' (bone|socket|none)")
    };
    var options = new ExportOptions(meshFormat: format, textureFormat: ETextureFormat.Png, exportMaterials: materials, socketFormat: socketFormat);
    var results = await session.RunAsync(Path.GetFullPath(outDir), options);
    foreach (var r in results)
    {
        if (r.Success)
            foreach (var f in r.DiskFilePaths ?? []) Console.WriteLine($"  ok    {f}");
        else
            Console.WriteLine($"  FAIL  {r.ObjectPath}: {r.Error?.Message}");
    }
    return results.Count > 0 && results.All(r => r.Success) ? 0 : 4;
}

// 动画：取 N 个均匀分布的帧（每段的中点），导出所有骨骼的局部变换（UE 坐标，未镜像，和游戏运行时一致）。
// 没有轨道的骨骼用参考姿势；平移模式为 Skeleton 的骨骼用参考平移；根骨骼固定为参考姿势（根运动在游戏里由角色胶囊体承担）
static int Anim(DefaultFileProvider p, string packagePath, string outFile, int count)
{
    var seq = p.LoadPackage(Resolve(p, packagePath)).GetExports().OfType<UAnimSequence>().FirstOrDefault()
        ?? throw new InvalidDataException($"no AnimSequence in {packagePath}");
    if (seq.AdditiveAnimType != EAdditiveAnimationType.AAT_None)
        throw new InvalidDataException($"{seq.Name} is additive ({seq.AdditiveAnimType})");
    var set = seq.ConvertAnims();
    var cs = set.Sequences[0];
    var refSk = set.Skeleton.ReferenceSkeleton;
    var tree = set.Skeleton.BoneTree;
    int n = refSk.FinalRefBoneInfo.Length, num = Math.Max(1, cs.NumFrames), k = Math.Clamp(count, 1, num);
    var frames = Enumerable.Range(0, k).Select(i => Math.Min(num - 1, (int)((i + 0.5) * num / k))).Distinct().ToArray();
    var modes = Enumerable.Range(0, n).GroupBy(b => b < tree.Length ? tree[b].ToString() : "n/a").ToDictionary(g => g.Key, g => g.Count());
    var tracked = Enumerable.Range(1, n - 1).Count(b => cs.OriginalSequence.FindTrackForBoneIndex(b) >= 0);
    var poses = new List<float[][]>();
    foreach (var f in frames)
    {
        var pose = new float[n][];
        for (var b = 0; b < n; b++)
        {
            var rp = refSk.FinalRefBonePose[b];
            FQuat q = rp.Rotation; FVector t = rp.Translation, s = rp.Scale3D;
            if (b > 0 && cs.OriginalSequence.FindTrackForBoneIndex(b) >= 0)
            {
                cs.Tracks[b].GetBoneTransform(f, num, ref q, ref t, ref s);
                if (b < tree.Length && tree[b] == EBoneTranslationRetargetingMode.Skeleton) t = rp.Translation;
            }
            pose[b] = [q.X, q.Y, q.Z, q.W, t.X, t.Y, t.Z, s.X, s.Y, s.Z];
        }
        poses.Add(pose);
    }
    var full = Path.GetFullPath(outFile);
    Directory.CreateDirectory(Path.GetDirectoryName(full)!);
    File.WriteAllText(full, JsonConvert.SerializeObject(new
    {
        name = seq.Name, numFrames = num, fps = cs.FramesPerSecond, frames,
        bones = refSk.FinalRefBoneInfo.Select(b => b.Name.Text).ToArray(),
        parents = refSk.FinalRefBoneInfo.Select(b => b.ParentIndex).ToArray(), poses
    }));
    Console.WriteLine($"{seq.Name}: {num} frames @ {cs.FramesPerSecond:0.#} fps, tracks {tracked}/{n}, frames [{string.Join(',', frames)}], translation modes {JsonConvert.SerializeObject(modes)} -> {full}");
    return 0;
}

// 接受完整路径，或省略 .uasset / .umap 后缀
static string Resolve(DefaultFileProvider p, string path)
{
    path = path.TrimStart('/');
    foreach (var candidate in new[] { path, path + ".uasset", path + ".umap" })
        if (p.Files.ContainsKey(candidate)) return candidate;
    throw new FileNotFoundException($"package not found: {path} (use `find` to locate it)");
}

static string Env(string name) =>
    Environment.GetEnvironmentVariable(name) is { Length: > 0 } v
        ? v
        : throw new InvalidOperationException($"environment variable {name} is not set; run `. tools\\env.ps1` first");

static string Arg(string[] a, int i, string name) =>
    a.Length > i && !a[i].StartsWith("--") ? a[i] : throw new ArgumentException($"missing <{name}>\n{Usage}");

static string? Opt(string[] a, string name)
{
    var i = Array.IndexOf(a, name);
    return i >= 0 && i + 1 < a.Length ? a[i + 1] : null;
}

static int Limit(string[] a) => Opt(a, "--limit") is { } s ? int.Parse(s) : 200;
