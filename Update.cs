using System;
using System.Diagnostics;
using System.IO;
using System.IO.Compression;
using System.Net;
using System.Reflection;
using System.Runtime.Serialization;
using System.Runtime.Serialization.Json;
using System.Security.Cryptography;
using System.Text;
using System.Threading;
using System.Windows.Forms;

[DataContract] sealed class ReleaseAsset
{
    [DataMember(Name="name")] public string Name;
    [DataMember(Name="browser_download_url")] public string Url;
    [DataMember(Name="digest")] public string Digest;
    [DataMember(Name="size")] public long Size;
}
[DataContract] sealed class ReleaseInfo
{
    [DataMember(Name="tag_name")] public string Tag;
    [DataMember(Name="draft")] public bool Draft;
    [DataMember(Name="prerelease")] public bool Prerelease;
    [DataMember(Name="assets")] public ReleaseAsset[] Assets;
    public ReleaseAsset Package;
    public Version Version;
}
static class Updates
{
    const string Repo="https://github.com/lemoncube7/better-than-copilot-key";
    const string AssetName="better-than-copilot-key-windows-x64.zip";
    const long MaxZip=32*1024*1024;
    static readonly string[] Executables={"차라리 이거.exe","copilot_key.exe"};
    public static Version Current {get{return Assembly.GetExecutingAssembly().GetName().Version;}}
    public static string VersionLabel {get{return Current.ToString(3);}}
    static HttpWebRequest Request(string url)
    {
        ServicePointManager.SecurityProtocol=SecurityProtocolType.Tls12;
        HttpWebRequest request=(HttpWebRequest)WebRequest.Create(url);
        request.UserAgent="BetterThanCopilotKey/"+VersionLabel;
        request.Accept="application/vnd.github+json";
        request.Timeout=20000;request.ReadWriteTimeout=20000;
        return request;
    }
    public static Version ParseVersion(string tag)
    {
        Version value;
        if(tag==null||!tag.StartsWith("v",StringComparison.Ordinal)||!Version.TryParse(tag.Substring(1),out value)||value.Build<0||value.Revision>0)
            throw new InvalidDataException("배포 버전 형식을 읽지 못했습니다.");
        return new Version(value.Major,value.Minor,value.Build,0);
    }
    public static ReleaseInfo ParseRelease(Stream stream)
    {
        ReleaseInfo release=(ReleaseInfo)new DataContractJsonSerializer(typeof(ReleaseInfo)).ReadObject(stream);
        if(release==null||release.Draft||release.Prerelease)throw new InvalidDataException("정식 배포 정보를 찾지 못했습니다.");
        release.Version=ParseVersion(release.Tag);
        if(release.Assets!=null)foreach(ReleaseAsset asset in release.Assets)if(asset.Name==AssetName)release.Package=asset;
        ReleaseAsset zip=release.Package;
        string expected=Repo+"/releases/download/"+release.Tag+"/"+AssetName;
        if(zip==null||zip.Url!=expected||zip.Size<=0||zip.Size>MaxZip||zip.Digest==null||zip.Digest.Length!=71||!zip.Digest.StartsWith("sha256:",StringComparison.Ordinal))
            throw new InvalidDataException("검증 가능한 Windows 배포 파일을 찾지 못했습니다.");
        for(int i=7;i<zip.Digest.Length;i++)if(!Uri.IsHexDigit(zip.Digest[i]))throw new InvalidDataException("잘못된 배포 파일 해시입니다.");
        return release;
    }
    public static ReleaseInfo Check()
    {
        using(WebResponse response=Request("https://api.github.com/repos/lemoncube7/better-than-copilot-key/releases/latest").GetResponse())
        using(Stream stream=response.GetResponseStream())return ParseRelease(stream);
    }
    static string Hash(string path)
    {
        using(SHA256 algorithm=SHA256.Create())using(Stream stream=File.OpenRead(path))
            return BitConverter.ToString(algorithm.ComputeHash(stream)).Replace("-","").ToLowerInvariant();
    }
    public static void ExtractPackage(string zipPath,string destination,string digest)
    {
        if("sha256:"+Hash(zipPath)!=digest.ToLowerInvariant())throw new InvalidDataException("다운로드 파일 검증에 실패했습니다. 다시 확인하세요.");
        Directory.CreateDirectory(destination);
        using(FileStream file=File.OpenRead(zipPath))using(ZipArchive zip=new ZipArchive(file,ZipArchiveMode.Read))
        {
            foreach(string name in Executables)
            {
                ZipArchiveEntry selected=null;
                foreach(ZipArchiveEntry entry in zip.Entries)if(entry.FullName==name){if(selected!=null)throw new InvalidDataException("중복 실행 파일입니다.");selected=entry;}
                if(selected==null||selected.Length<2||selected.Length>MaxZip)throw new InvalidDataException("업데이트 실행 파일이 없습니다.");
                string target=Path.Combine(destination,name);
                using(Stream input=selected.Open())using(FileStream output=new FileStream(target,FileMode.CreateNew))
                {
                    byte[] bytes=new byte[65536];long count=0;int n;
                    while((n=input.Read(bytes,0,bytes.Length))>0){count+=n;if(count>MaxZip)throw new InvalidDataException("실행 파일이 너무 큽니다.");output.Write(bytes,0,n);}
                }
                using(FileStream image=File.OpenRead(target))if(image.ReadByte()!=77||image.ReadByte()!=90)throw new InvalidDataException("Windows 실행 파일이 아닙니다.");
            }
        }
    }
    public static string Download(ReleaseInfo release)
    {
        string temp=Path.Combine(Path.GetTempPath(),"BetterThanCopilot-update-"+Guid.NewGuid().ToString("N"));Directory.CreateDirectory(temp);
        string zip=Path.Combine(temp,"package.zip");
        using(WebResponse response=Request(release.Package.Url).GetResponse())using(Stream input=response.GetResponseStream())using(FileStream output=File.Create(zip))
        {
            byte[] bytes=new byte[65536];long count=0;int n;
            while((n=input.Read(bytes,0,bytes.Length))>0){count+=n;if(count>MaxZip)throw new InvalidDataException("다운로드 파일이 너무 큽니다.");output.Write(bytes,0,n);}
            if(count!=release.Package.Size)throw new InvalidDataException("다운로드 파일 크기가 맞지 않습니다.");
        }
        string staged=Path.Combine(temp,"files");ExtractPackage(zip,staged,release.Package.Digest);
        Version candidate=AssemblyName.GetAssemblyName(Path.Combine(staged,Executables[0])).Version;
        if(candidate!=release.Version)throw new InvalidDataException("실행 파일 버전과 배포 버전이 다릅니다.");
        File.Delete(zip);return temp;
    }
    static string Encode(string value){return Convert.ToBase64String(Encoding.UTF8.GetBytes(value));}
    public static void LaunchInstaller(string temp)
    {
        string helper=Path.Combine(temp,"installer.exe");File.Copy(Application.ExecutablePath,helper);
        Process.Start(new ProcessStartInfo(helper,"--apply-update "+Encode(temp)+" "+Encode(AppDomain.CurrentDomain.BaseDirectory)+" "+Process.GetCurrentProcess().Id)
            {UseShellExecute=false,CreateNoWindow=true});
        DateTime deadline=DateTime.UtcNow.AddSeconds(10);
        while(!File.Exists(Path.Combine(temp,"installer.ready"))){if(DateTime.UtcNow>deadline)throw new IOException("업데이트 도우미가 준비되지 않았습니다.");Thread.Sleep(50);}
    }
    public static void ReplaceFiles(string staged,string target)
    {
        string suffix=".before-update-"+Guid.NewGuid().ToString("N");
        int copied=0,moved=0;
        try
        {
            foreach(string name in Executables)
            {
                string path=Path.Combine(target,name);
                if(!File.Exists(path))throw new FileNotFoundException("설치된 실행 파일이 없습니다.",path);
                File.Move(path,path+suffix);moved++;
                File.Copy(Path.Combine(staged,name),path);copied++;
            }
        }
        catch
        {
            for(int i=moved-1;i>=0;i--){string path=Path.Combine(target,Executables[i]);if(File.Exists(path))File.Delete(path);File.Move(path+suffix,path);}
            throw;
        }
        for(int i=0;i<copied;i++){try{File.Delete(Path.Combine(target,Executables[i])+suffix);}catch(IOException){}}
    }
    public static void Install(string[] args)
    {
        string temp=Path.GetFullPath(Encoding.UTF8.GetString(Convert.FromBase64String(args[1])));
        string target=Path.GetFullPath(Encoding.UTF8.GetString(Convert.FromBase64String(args[2])));
        string staged=Path.Combine(temp,"files");string app=Path.Combine(target,Executables[0]);
        Process parent=Process.GetProcessById(Int32.Parse(args[3]));
        if(!String.Equals(Path.GetFullPath(parent.MainModule.FileName),app,StringComparison.OrdinalIgnoreCase))throw new InvalidOperationException("업데이트 대상 프로세스가 다릅니다.");
        File.WriteAllText(Path.Combine(temp,"installer.ready"),"ready");
        if(!parent.WaitForExit(15000))throw new IOException("설정 창이 종료되지 않아 업데이트를 중단했습니다.");
        try
        {
            ReplaceFiles(staged,target);
            Process.Start(new ProcessStartInfo(app){UseShellExecute=true});
        }
        catch(Exception e)
        {
            MessageBox.Show("업데이트하지 못했습니다. 기존 버전을 유지합니다.\r\n"+e.Message,"차라리 이거");
            if(File.Exists(app))Process.Start(new ProcessStartInfo(app){UseShellExecute=true});
        }
    }
    public static void SelfTest(string directory)
    {
        Directory.CreateDirectory(directory);
        if(ParseVersion("v0.2.3")!=new Version(0,2,3,0))throw new Exception("Version parse");
        string zip=Path.Combine(directory,"test.zip"),stage=Path.Combine(directory,"stage"),target=Path.Combine(directory,"target");
        using(FileStream file=File.Create(zip))using(ZipArchive archive=new ZipArchive(file,ZipArchiveMode.Create))
        {
            foreach(string name in Executables){using(Stream stream=archive.CreateEntry(name).Open()){byte[] data=Encoding.UTF8.GetBytes("MZnew-"+name);stream.Write(data,0,data.Length);}}
            using(Stream stream=archive.CreateEntry("../escape.txt").Open())stream.WriteByte(42);
            using(Stream stream=archive.CreateEntry("settings.json").Open())stream.WriteByte(42);
        }
        ExtractPackage(zip,stage,"sha256:"+Hash(zip));
        if(File.Exists(Path.Combine(directory,"escape.txt"))||File.Exists(Path.Combine(stage,"settings.json")))throw new Exception("ZIP allowlist");
        bool rejected=false;try{ExtractPackage(zip,Path.Combine(directory,"bad"),"sha256:"+new string('0',64));}catch(InvalidDataException){rejected=true;}if(!rejected)throw new Exception("Digest check");
        Directory.CreateDirectory(target);File.WriteAllText(Path.Combine(target,"settings.json"),"personal");
        foreach(string name in Executables)File.WriteAllText(Path.Combine(target,name),"old-"+name);
        File.Delete(Path.Combine(stage,Executables[1]));
        rejected=false;try{ReplaceFiles(stage,target);}catch(FileNotFoundException){rejected=true;}
        if(!rejected||File.ReadAllText(Path.Combine(target,Executables[0]))!="old-"+Executables[0]||File.ReadAllText(Path.Combine(target,Executables[1]))!="old-"+Executables[1])throw new Exception("Rollback");
        File.WriteAllText(Path.Combine(stage,Executables[1]),"MZnew");ReplaceFiles(stage,target);
        if(File.ReadAllText(Path.Combine(target,"settings.json"))!="personal"||File.ReadAllText(Path.Combine(target,Executables[1]))!="MZnew")throw new Exception("Settings preservation");
        File.WriteAllText(Path.Combine(directory,"update-test.txt"),"PASS: versions, digest, ZIP allowlist, rollback, settings preservation");
    }
}
