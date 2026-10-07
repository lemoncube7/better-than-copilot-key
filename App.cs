using System;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.IO.Pipes;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using System.Windows.Forms;

static class App
{
    [DllImport("user32.dll")] static extern bool SetProcessDpiAwarenessContext(IntPtr context);
    public static void Preview(EffectSettings settings)
    {
        string engine = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "copilot_key.exe");
        if (!File.Exists(engine)) throw new FileNotFoundException("같은 폴더에 copilot_key.exe가 있어야 합니다.");
        string payload = Convert.ToBase64String(Encoding.UTF8.GetBytes(EffectSettings.Encode(settings)));
        Process.Start(new ProcessStartInfo(engine, "--payload " + payload) { UseShellExecute = false, CreateNoWindow = true });
    }
    [STAThread] static void Main(string[] args)
    {
        try
        {
            try { SetProcessDpiAwarenessContext(new IntPtr(-4)); } catch (EntryPointNotFoundException) { }
            Application.SetCompatibleTextRenderingDefault(false);
            Application.EnableVisualStyles();
            SettingsWindow window = new SettingsWindow();
            int test = Array.IndexOf(args, "--ui-test");
            if (test >= 0) window.VerifyOnShown(args[test + 1]);
            Application.Run(window);
        }
        catch (Exception e)
        {
            Environment.ExitCode = 1;
            MessageBox.Show(e.Message, "차라리 이거", MessageBoxButtons.OK, MessageBoxIcon.Error);
        }
    }
}
sealed class SettingsWindow : Form
{
    readonly ChoiceControl effect = new ChoiceControl(), palette = new ChoiceControl();
    ChoiceControl fire;
    readonly FlowLayoutPanel controls = new FlowLayoutPanel();
    readonly Label status = new Label();
    EffectSettings settings;
    bool initializing;
    readonly Color bg = Color.FromArgb(22, 25, 34), panel = Color.FromArgb(32, 37, 49), fg = Color.FromArgb(230, 234, 244);
    public SettingsWindow()
    {
        int height = Math.Max(480, Math.Min(1120, Screen.FromPoint(Cursor.Position).WorkingArea.Height - 80));
        Text = "차라리 이거 · 효과 설정"; Size = new Size(700, height); MinimumSize = new Size(640, Math.Min(920, height));
        StartPosition = FormStartPosition.CenterScreen; BackColor = bg; ForeColor = fg;
        Font = new Font("맑은 고딕", 10); AutoScaleMode = AutoScaleMode.Dpi;
        try { settings = EffectSettings.Load(); } catch (Exception e) { settings = new EffectSettings(); status.Text = "설정을 읽지 못해 기본값을 표시합니다: " + e.Message; }
        TableLayoutPanel root = new TableLayoutPanel { Dock = DockStyle.Fill, Padding = new Padding(26), ColumnCount = 1, RowCount = 8 };
        root.RowStyles.Add(new RowStyle(SizeType.Absolute, 48)); root.RowStyles.Add(new RowStyle(SizeType.Absolute, 42));
        root.RowStyles.Add(new RowStyle(SizeType.Absolute, 60)); root.RowStyles.Add(new RowStyle(SizeType.Absolute, 55));
        root.RowStyles.Add(new RowStyle(SizeType.Percent, 100)); root.RowStyles.Add(new RowStyle(SizeType.Absolute, 65));
        root.RowStyles.Add(new RowStyle(SizeType.Absolute, 55)); root.RowStyles.Add(new RowStyle(SizeType.Absolute, 48));
        root.Controls.Add(new Label { Text = "버튼 하나, 잠깐의 즐거움", Font = new Font(Font.FontFamily, 20, FontStyle.Bold), Dock = DockStyle.Fill }, 0, 0);
        root.Controls.Add(new Label { Text = "Copilot 대신, 심심할 때 만지작. 효과를 고르고 조정하세요.", Dock = DockStyle.Fill, ForeColor = Color.LightSteelBlue }, 0, 1);
        ConfigureCombo(effect, new string[] { "색종이 폭죽", "커서 중력장", "별가루 클러스터", "블랙홀 · hold", "진자 · 놓으면 발사", "십자가 섬광 · hold", "레일건 · 휠 충전", "주사위 · 흔들어서 굴리기" });
        effect.SelectedIndex = Array.IndexOf(new string[] { "confetti", "gravity", "cluster", "blackhole", "pendulum", "crossflash", "railgun", "dice" }, settings.Effect);
        root.Controls.Add(Row("Copilot 키 효과", effect), 0, 2);
        ConfigureCombo(palette, new string[] { "네온", "파스텔", "차가운 별빛", "따뜻한 불꽃" });
        palette.SelectedIndex = Array.IndexOf(new string[] { "neon", "pastel", "ice", "warm" }, settings.Palette);
        root.Controls.Add(Row("색상", palette), 0, 3);
        controls.Dock = DockStyle.Fill; controls.FlowDirection = FlowDirection.TopDown; controls.WrapContents = false; controls.AutoScroll = true;
        root.Controls.Add(controls, 0, 4);
        Label note = new Label { Dock = DockStyle.Fill, ForeColor = Color.LightSteelBlue,
            Text = "최상단 · 클릭 통과 · Esc로 종료 · 효과 창 하나\r\n레일건 hold 중 휠·가운데 클릭 차단. 그 외 클릭은 통과합니다." };
        root.Controls.Add(note, 0, 5);
        FlowLayoutPanel buttons = new FlowLayoutPanel { Dock = DockStyle.Fill, WrapContents = false };
        Button preview = Button("미리보기", Color.FromArgb(96, 93, 230));
        Button save = Button("저장", Color.FromArgb(43, 100, 129));
        Button reset = Button("기본값 복원", panel);
        buttons.Controls.Add(preview); buttons.Controls.Add(save); buttons.Controls.Add(reset); root.Controls.Add(buttons, 0, 6);
        status.Dock = DockStyle.Fill; status.ForeColor = Color.LightSteelBlue; root.Controls.Add(status, 0, 7);
        Controls.Add(root);
        BuildEffectControls();
        effect.SelectedIndexChanged += delegate { settings.Effect = new string[] { "confetti", "gravity", "cluster", "blackhole", "pendulum", "crossflash", "railgun", "dice" }[effect.SelectedIndex]; BuildEffectControls(); status.Text = "변경 후 저장하면 Copilot 키에 적용됩니다."; };
        palette.SelectedIndexChanged += delegate { settings.Palette = new string[] { "neon", "pastel", "ice", "warm" }[palette.SelectedIndex]; };
        preview.Click += delegate { try { App.Preview(settings); status.Text = "현재 값으로 미리보기 중 · 저장된 설정은 바뀌지 않습니다."; } catch (Exception e) { status.Text = e.Message; } };
        save.Click += delegate { try { EffectSettings.Save(settings); status.Text = "저장했습니다. 다음 Copilot 키 실행부터 적용됩니다."; } catch (Exception e) { status.Text = "저장 실패: " + e.Message; } };
        reset.Click += delegate { settings = new EffectSettings(); initializing = true; effect.SelectedIndex = 2; palette.SelectedIndex = 0; initializing = false; BuildEffectControls(); status.Text = "기본값으로 복원했습니다. 저장하면 적용됩니다."; };
        if (status.Text == "") status.Text = "미리보기로 시험한 뒤 저장하세요.";
    }
    void ConfigureCombo(ChoiceControl box, string[] items)
    {
        box.Items.AddRange(items); box.Width = 290;
        box.BackColor = panel; box.ForeColor = fg;
    }
    Control Row(string text, Control input)
    {
        TableLayoutPanel row = new TableLayoutPanel { Width = 590, Height = 48, ColumnCount = 2, Dock = DockStyle.Top, Margin = new Padding(0, 4, 0, 4) };
        row.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 220)); row.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        row.Controls.Add(new Label { Text = text, Dock = DockStyle.Fill, TextAlign = ContentAlignment.MiddleLeft }, 0, 0); input.Anchor = AnchorStyles.Left; row.Controls.Add(input, 1, 0); return row;
    }
    Button Button(string text, Color color) { return new Button { Text = text, Size = new Size(150, 42), FlatStyle = FlatStyle.Flat, BackColor = color, ForeColor = fg, Margin = new Padding(0, 0, 12, 0) }; }
    NumericUpDown Number(string label, double value, decimal min, decimal max, int decimals, decimal step, Action<double> set)
    {
        NumericUpDown n = new NumericUpDown { Minimum = min, Maximum = max, DecimalPlaces = decimals, Increment = step, Value = (decimal)value, Width = 150, BackColor = panel, ForeColor = fg };
        n.ValueChanged += delegate { set((double)n.Value); }; controls.Controls.Add(Row(label, n)); return n;
    }
    void BuildEffectControls()
    {
        if (initializing) return;
        while (controls.Controls.Count > 0) controls.Controls[0].Dispose();
        if (settings.Effect == "confetti")
        {
            Number("입자 수 / 모니터", settings.ConfettiCount, 20, 1200, 0, 20, delegate(double v) { settings.ConfettiCount = (int)v; });
            Number("지속 시간 (초)", settings.ConfettiLife, .5m, 10, 1, .1m, delegate(double v) { settings.ConfettiLife = v; });
            Number("발사 속도 배율", settings.ConfettiSpeed, .2m, 3, 1, .1m, delegate(double v) { settings.ConfettiSpeed = v; });
        }
        else if (settings.Effect == "gravity")
        {
            Number("별가루 수", settings.GravityCount, 10, 800, 0, 10, delegate(double v) { settings.GravityCount = (int)v; });
            Number("최대 수명 (초)", settings.GravityLife, 1, 30, 1, .5m, delegate(double v) { settings.GravityLife = v; });
            Number("중력 세기 배율", settings.GravityStrength, .1m, 5, 1, .1m, delegate(double v) { settings.GravityStrength = v; });
            Number("충돌 반경 (px)", settings.CollisionRadius, 4, 60, 0, 1, delegate(double v) { settings.CollisionRadius = v; });
        }
        else if (settings.Effect == "blackhole")
        {
            Number("초기 생성 수 / 0.8초", settings.BlackHoleCount, 30, 800, 0, 10, delegate(double v) { settings.BlackHoleCount = (int)v; });
            Number("지속 생성 수 / 0.35초", settings.BlackHoleFeedCount, 0, 200, 0, 5, delegate(double v) { settings.BlackHoleFeedCount = (int)v; });
            Number("흡인 세기", settings.BlackHoleStrength, .1m, 5, 1, .1m, delegate(double v) { settings.BlackHoleStrength = v; });
            Number("성장 속도 배율", settings.BlackHoleGrowth, .01m, 1, 2, .01m, delegate(double v) { settings.BlackHoleGrowth = v; });
            Number("궤도 감속", settings.BlackHoleOrbitDamping, 0, .3m, 3, .005m, delegate(double v) { settings.BlackHoleOrbitDamping = v; });
            Number("공전 비율", settings.BlackHoleOrbitRatio, 0, 1, 2, .05m, delegate(double v) { settings.BlackHoleOrbitRatio = v; });
            FlightGravity();
            controls.Controls.Add(new Label { Text = "별이 화면 전체에 서서히 나타납니다.\r\n누르는 동안 모으고, 놓으면 모은 개수만큼 방출합니다.", Width = 560, Height = 65, ForeColor = Color.LightSteelBlue });
        }
        else if (settings.Effect == "pendulum")
        {
            Number("줄 길이 (px)", settings.PendulumLength, 80, 360, 0, 10, delegate(double v) { settings.PendulumLength = v; });
            Number("진자 중력 (px/s²)", settings.PendulumGravity, 200, 3000, 0, 100, delegate(double v) { settings.PendulumGravity = v; });
            Number("마찰", settings.PendulumDamping, 0, 1, 2, .02m, delegate(double v) { settings.PendulumDamping = v; });
            CheckBox elastic = new CheckBox { Text = "적용", Checked = settings.PendulumElasticEnabled, AutoSize = true, ForeColor = fg };
            controls.Controls.Add(Row("줄 탄성", elastic));
            NumericUpDown stiffness = Number("줄 단단함", settings.PendulumStiffness, 10, 300, 0, 10, delegate(double v) { settings.PendulumStiffness = v; });
            NumericUpDown stretch = Number("최대 늘어남 (배)", settings.PendulumMaxStretch, 1.2m, 5, 1, .1m, delegate(double v) { settings.PendulumMaxStretch = v; });
            stiffness.Enabled = stretch.Enabled = elastic.Checked;
            elastic.CheckedChanged += delegate { settings.PendulumElasticEnabled = elastic.Checked; stiffness.Enabled = stretch.Enabled = elastic.Checked; };
            Number("발사 후 폭발까지 (초)", settings.ClusterFuse, .1m, 5, 1, .1m, delegate(double v) { settings.ClusterFuse = v; });
            FlightGravity();
            controls.Controls.Add(new Label { Text = "매다는 지점이 커서를 따라옵니다. 흔들어서 가속하세요.\r\n놓으면 커서 이동 속도까지 합쳐서 날아갑니다.", Width = 560, Height = 65, ForeColor = Color.LightSteelBlue });
        }
        else if (settings.Effect == "dice")
        {
            Number("주사위 크기 (px)", settings.DiceSize, 40, 140, 0, 4, delegate(double v) { settings.DiceSize = v; });
            Number("흔들림 민감도", settings.DiceSensitivity, .3m, 3, 1, .1m, delegate(double v) { settings.DiceSensitivity = v; });
            Number("정지 감지 시간 (초)", settings.DiceStopDelay, .08m, .6m, 2, .02m, delegate(double v) { settings.DiceStopDelay = v; });
            controls.Controls.Add(new Label { Text = "누른 채 마우스를 흔들면 회전합니다. 멈추면 한 면으로 정렬됩니다.\r\n놓으면 결과를 1.5초 보여주고 종료합니다.", Width = 560, Height = 65, ForeColor = Color.LightSteelBlue });
        }
        else if (settings.Effect == "railgun")
        {
            Number("휠 한 칸당 충전 (%)", settings.RailChargePerNotch, 1, 25, 0, 1, delegate(double v) { settings.RailChargePerNotch = v; });
            Number("발사 위력 배율", settings.RailPower, .5m, 2, 1, .1m, delegate(double v) { settings.RailPower = v; });
            Number("충돌 파편 수", settings.RailImpactCount, 50, 800, 0, 50, delegate(double v) { settings.RailImpactCount = (int)v; });
            controls.Controls.Add(new Label { Text = "hold 시작 위치에 설치 · 휠 클릭으로 추가 설치 (최대 16개)\r\n휠로 함께 충전 · 마우스로 조준 · 놓으면 동시 발사", Width = 560, Height = 65, ForeColor = Color.LightSteelBlue });
        }
        else if (settings.Effect == "crossflash")
        {
            Number("충전 속도", settings.CrossGatherRate, 20, 400, 0, 20, delegate(double v) { settings.CrossGatherRate = v; });
            Number("십자가 반길이 (px)", settings.CrossReach, 100, 900, 0, 25, delegate(double v) { settings.CrossReach = v; });
            Number("기본 폭발 입자 수", settings.CrossBurstCount, 50, 1200, 0, 50, delegate(double v) { settings.CrossBurstCount = (int)v; });
            FlightGravity();
            controls.Controls.Add(new Label { Text = "누르는 동안 흰 코어의 십자 광선이 밝고 두꺼워집니다.\r\n놓으면 십자 전체가 번쩍하며 터집니다. Esc로 취소.", Width = 560, Height = 65, ForeColor = Color.LightSteelBlue });
        }
        else
        {
            Number("폭발 후 별가루 수", settings.ClusterCount, 20, 1000, 0, 10, delegate(double v) { settings.ClusterCount = (int)v; });
            Number("폭발까지 (초)", settings.ClusterFuse, .1m, 5, 1, .1m, delegate(double v) { settings.ClusterFuse = v; });
            if (settings.FireMode == "slingshot") Number("새총 세기", settings.SlingshotPower, 1, 15, 1, .5m, delegate(double v) { settings.SlingshotPower = v; });
            else Number("마우스 속도 배율", settings.ClusterSpeed, .1m, 4, 1, .1m, delegate(double v) { settings.ClusterSpeed = v; });
            fire = new ChoiceControl(); ConfigureCombo(fire, new string[] { "실행 즉시 발사", "가운데 클릭으로 발사", "왼쪽 클릭으로 발사", "Space로 발사", "새총 · Copilot hold" });
            fire.SelectedIndex = Array.IndexOf(new string[] { "instant", "middle", "left", "space", "slingshot" }, settings.FireMode);
            controls.Controls.Add(Row("발사 방식", fire));
            fire.SelectedIndexChanged -= FireChanged; fire.SelectedIndexChanged += FireChanged;
            FlightGravity();
            controls.Controls.Add(new Label { Text = settings.FireMode == "slingshot" ? "Copilot 키를 누른 채 마우스로 당기고, 키를 놓으면 발사.\r\n당긴 반대 방향으로 날아갑니다. Esc로 취소." : "화면 끝에 닿으면 즉시 폭발합니다.\r\n클릭·Space 모드: 커서 옆에서 대기, 8초 무입력 시 종료.", Width = 560, Height = 65, ForeColor = Color.LightSteelBlue });
        }
    }
    void FlightGravity()
    {
        CheckBox gravity = new CheckBox { Text = "적용", Checked = settings.ClusterGravityEnabled, AutoSize = true, ForeColor = fg };
        gravity.CheckedChanged += delegate { settings.ClusterGravityEnabled = gravity.Checked; };
        controls.Controls.Add(Row("낙하 중력", gravity));
        Number("중력 세기 (px/s²)", settings.ClusterGravity, 50, 3000, 0, 50, delegate(double v) { settings.ClusterGravity = v; });
    }
    void FireChanged(object sender, EventArgs args)
    {
        if (fire.SelectedIndex >= 0) { settings.FireMode = new string[] { "instant", "middle", "left", "space", "slingshot" }[fire.SelectedIndex]; BuildEffectControls(); }
    }
    public void VerifyOnShown(string directory)
    {
        Shown += delegate
        {
            System.Windows.Forms.Timer check = new System.Windows.Forms.Timer { Interval = 350 };
            check.Tick += delegate
            {
                check.Stop();
                try
                {
                    Directory.CreateDirectory(directory);
                    for (int i = 0; i < 8; i++)
                    {
                        effect.VerifyMenuSelection(i); PerformLayout(); controls.PerformLayout();
                        using (Bitmap bitmap = new Bitmap(Width, Height)) { DrawToBitmap(bitmap, new Rectangle(0, 0, Width, Height)); bitmap.Save(Path.Combine(directory, "settings-" + i + ".png")); }
                    }
                    effect.VerifyMenuSelection(4);
                    foreach (Control row in controls.Controls)
                        foreach (Control child in row.Controls)
                        {
                            CheckBox elasticCheck = child as CheckBox;
                            if (elasticCheck != null && row.Controls[0].Text == "줄 탄성") elasticCheck.Checked = true;
                        }
                    EffectSettings.Save(settings);
                    if (!EffectSettings.Load().PendulumElasticEnabled) throw new Exception("Elastic setting save failed");
                    PerformLayout(); controls.PerformLayout();
                    using (Bitmap bitmap = new Bitmap(Width, Height)) { DrawToBitmap(bitmap, new Rectangle(0, 0, Width, Height)); bitmap.Save(Path.Combine(directory, "elastic-settings.png")); }
                    effect.VerifyMenuSelection(3);
                    foreach (Control row in controls.Controls)
                    {
                        foreach (Control child in row.Controls)
                        {
                            NumericUpDown number = child as NumericUpDown;
                            if (number != null && row.Controls[0].Text == "지속 생성 수 / 0.35초") number.Value = 40;
                        }
                    }
                    EffectSettings.Save(settings);
                    if (EffectSettings.Load().BlackHoleFeedCount != 40) throw new Exception("Continuous generation save failed");
                    settings.BlackHoleFeedCount = 0; EffectSettings.Save(settings);
                    if (EffectSettings.Load().BlackHoleFeedCount != 0) throw new Exception("Continuous generation zero failed");
                    effect.VerifyMenuSelection(2);
                    for (int round = 0; round < 4; round++)
                    {
                        palette.VerifyMenuSelection(round);
                        effect.VerifyMenuSelection(round % 3);
                    }
                    effect.VerifyMenuSelection(2);
                    fire.VerifyMenuSelection(1);
                    if (settings.FireMode != "middle") throw new Exception("Fire selection failed");
                    fire.VerifyMenuSelection(4);
                    if (settings.FireMode != "slingshot" || settings.SlingshotPower < 1 || settings.SlingshotPower > 15) throw new Exception("Slingshot selection failed");
                    foreach (Control row in controls.Controls)
                    {
                        foreach (Control child in row.Controls)
                        {
                            CheckBox checkBox = child as CheckBox;
                            if (checkBox != null) checkBox.Checked = true;
                            NumericUpDown number = child as NumericUpDown;
                            if (number != null && row.Controls[0].Text == "중력 세기 (px/s²)") number.Value = 900;
                        }
                    }
                    PerformLayout(); controls.PerformLayout();
                    using (Bitmap bitmap = new Bitmap(Width, Height)) { DrawToBitmap(bitmap, new Rectangle(0, 0, Width, Height)); bitmap.Save(Path.Combine(directory, "slingshot-settings.png")); }
                    fire.VerifyMenuSelection(1);
                    EffectSettings.Save(settings);
                    if (EffectSettings.Load().FireMode != "middle") throw new Exception("Saved selection failed");
                    if (!EffectSettings.Load().ClusterGravityEnabled || EffectSettings.Load().ClusterGravity != 900) throw new Exception("Saved gravity failed");
                    // Restore a sensible first-run default after the test.
                    EffectSettings.Save(new EffectSettings());
                    var watch = Stopwatch.StartNew();
                    App.Preview(new EffectSettings()); long first = watch.ElapsedMilliseconds;
                    watch.Restart(); App.Preview(new EffectSettings()); long repeat = watch.ElapsedMilliseconds;
                    File.WriteAllText(Path.Combine(directory, "ui-test.txt"), "Menu cycles, slingshot, gravity checkbox and value, persistence: PASS\r\nPreview dispatch ms: " + first + ", " + repeat);
                }
                catch (Exception e) { Environment.ExitCode = 1; File.WriteAllText(Path.Combine(directory, "ui-test.txt"), e.ToString()); }
                finally { check.Dispose(); Close(); }
            };
            check.Start();
        };
    }
}

sealed class ChoiceControl : UserControl
{
    readonly Button button = new Button();
    ContextMenuStrip menu;
    int index = -1;
    public readonly System.Collections.Generic.List<string> Items = new System.Collections.Generic.List<string>();
    public event EventHandler SelectedIndexChanged;
    public int SelectedIndex
    {
        get { return index; }
        set
        {
            if (index == value) return;
            index = value; button.Text = (index >= 0 && index < Items.Count ? Items[index] : "선택") + "    ▾";
            if (SelectedIndexChanged != null) SelectedIndexChanged(this, EventArgs.Empty);
        }
    }
    public ChoiceControl()
    {
        Height = 36; button.Dock = DockStyle.Fill; button.FlatStyle = FlatStyle.Flat;
        button.TextAlign = ContentAlignment.MiddleLeft; button.Padding = new Padding(8, 0, 0, 0);
        Controls.Add(button);
        button.Click += delegate
        {
            if (menu != null)
            {
                for (int i = 0; i < menu.Items.Count; i++) ((ToolStripMenuItem)menu.Items[i]).Checked = i == index;
                menu.Show(this, new Point(0, Height)); return;
            }
            menu = new ContextMenuStrip { BackColor = BackColor, ForeColor = ForeColor, Font = Font };
            for (int i = 0; i < Items.Count; i++)
            {
                int selected = i;
                ToolStripMenuItem item = new ToolStripMenuItem(Items[i]) { Checked = i == index, ForeColor = ForeColor };
                item.Click += delegate
                {
                    // Selection can rebuild/dispose controls. Finish menu dispatch first.
                    BeginInvoke((MethodInvoker)delegate { if (!IsDisposed) SelectedIndex = selected; });
                };
                menu.Items.Add(item);
            }
            menu.Show(this, new Point(0, Height));
        };
    }
    protected override void Dispose(bool disposing)
    {
        if (disposing && menu != null) { menu.Dispose(); menu = null; }
        base.Dispose(disposing);
    }
    public void VerifyMenuSelection(int selected)
    {
        button.PerformClick();
        ((ToolStripMenuItem)menu.Items[selected]).PerformClick();
        menu.Close(); Application.DoEvents();
        if (SelectedIndex != selected) throw new Exception("Menu selection failed");
    }
}




