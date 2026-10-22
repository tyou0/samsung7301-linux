/*
 * libfprint-samsung7301-afis -- SourceAFIS matcher helper for the
 * samsung7301 libfprint driver.
 *
 * The driver captures native 160x160 frames of the sensor and hands them to
 * this helper over a pipe.  The helper extracts real SourceAFIS templates
 * from those frames and prints exactly one result line on stdout:
 *
 *   OK <quality_max> <best_cross> <selected_count> <i0> <i1> <i2> <i3>
 *
 * Every failure prints nothing on stdout and exits with a non zero status.
 *
 * Input (stdin, exact, little endian):
 *
 *   offset  0   8 bytes   magic, ASCII "S731AF01"
 *   offset  8   u32       probe_count      (1..32)
 *   offset 12   u32       reference_count  (0..32)
 *   offset 16            probe_count frames of 25600 bytes
 *                        reference_count frames of 25600 bytes
 *   end                 the stream has to end here, trailing bytes are an
 *                       error
 *
 * The frames are the native sensor frames, 160x160 8 bit grey, without any
 * header and without any resizing.  The helper does the image preparation
 * itself, exactly as it was validated: a per frame min/max linear contrast
 * stretch with integer rounding, then the inversion (255 - x), then
 * SourceAFIS at 500 dpi.
 *
 * Scoring:
 *
 *   self(p)   the exact SourceAFIS self match of probe p, that is
 *              Match(template(p), template(p)).  It measures how well the
 *              finger was captured and is used as capture strength for the
 *              selection only.  It is never used as an identity decision.
 *   cross(p)  the exact SourceAFIS score of probe p against the reference
 *              frames.  This is the identity measure.
 *
 * A probe is eligible when self(p) >= 40.  With references present a probe
 * is additionally only selected when cross(p) >= 40, so that a different
 * finger can never be mixed into a template.
 *
 * The helper keeps every template in RAM only.  Nothing is serialized, no
 * file is read or written, no network is used and no image codec is
 * involved.  A SourceAFIS extraction or match error aborts the whole run
 * with a non zero exit status; it is never turned into a score.
 *
 * Copyright (C) 2026 samsung7301-linux contributors
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

using System.Globalization;
using System.Text;
using SourceAFIS;

namespace SourceAfisMatcher;

internal static class Program
{
    /* Geometry of the native sensor frame. */
    private const int Width = 160;
    private const int Height = 160;
    private const int FrameBytes = Width * Height;          /* 25600 */

    /* Bounds of the wire protocol. */
    private const int MaxFrames = 32;
    private const int MaxSelected = 4;
    private const int HeaderBytes = 16;

    /* A frame whose raw standard deviation is below this carries no ridge
     * information at all and is unusable. */
    private const double BlankStdDevThreshold = 2.0;

    /* SourceAFIS's own threshold.  It is not lowered anywhere. */
    private const double MatchThreshold = 40.0;

    private static readonly byte[] Magic = Encoding.ASCII.GetBytes("S731AF01");

    private static int Main()
    {
        try
        {
            return Run();
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"error: {ex.GetType().Name}: {ex.Message}");
            Console.Error.Flush();
            return 1;
        }
    }

    private static int Run()
    {
        var input = ReadInput();
        uint probeCount = ReadU32Le(input, 8);
        uint referenceCount = ReadU32Le(input, 12);

        var probes = new byte[probeCount][];
        for (uint i = 0; i < probeCount; i++)
            probes[i] = Slice(input, HeaderBytes + (long)i * FrameBytes, FrameBytes);

        var references = new byte[referenceCount][];
        for (uint i = 0; i < referenceCount; i++)
            references[i] = Slice(input,
                                  HeaderBytes + ((long)probeCount + i) * FrameBytes,
                                  FrameBytes);

        /* Extract the real templates once. */
        var probeTemplates = Extract(probes);
        var referenceTemplates = Extract(references);

        /* Capture strength: the exact self match of the probe. */
        var self = new double[probeCount];
        var eligible = new bool[probeCount];
        double qualityMax = 0.0;

        for (uint p = 0; p < probeCount; p++)
        {
            if (probeTemplates[p] is null)
                continue;

            self[p] = Match(probeTemplates[p]!, probeTemplates[p]!);
            if (self[p] > qualityMax)
                qualityMax = self[p];
            if (self[p] >= MatchThreshold)
                eligible[p] = true;
        }

        /* Identity: the exact score of an eligible probe against the
         * references. */
        var cross = new double[probeCount];
        double bestCross = 0.0;

        for (uint p = 0; p < probeCount; p++)
        {
            if (!eligible[p])
                continue;

            double best = 0.0;
            for (uint r = 0; r < referenceCount; r++)
            {
                if (referenceTemplates[r] is null)
                    continue;

                double score = Match(probeTemplates[p]!, referenceTemplates[r]!);
                if (score > best)
                    best = score;
            }
            cross[p] = best;
            if (best > bestCross)
                bestCross = best;
        }

        /* Selection: the strongest probes, at most four of them, each of them
         * a distinct frame.  With references present a probe has to match
         * them as well, so that a different finger is never enrolled. */
        var order = new List<uint>();
        for (uint p = 0; p < probeCount; p++)
        {
            if (!eligible[p])
                continue;
            if (referenceCount > 0 && cross[p] < MatchThreshold)
                continue;
            order.Add(p);
        }
        order.Sort((a, b) =>
        {
            int byScore = self[b].CompareTo(self[a]);
            return byScore != 0 ? byScore : a.CompareTo(b);
        });

        var selected = new List<int>();
        var selectedBytes = new List<byte[]>();
        foreach (uint p in order)
        {
            if (selected.Count >= MaxSelected)
                break;

            bool duplicate = false;
            foreach (var taken in selectedBytes)
            {
                if (taken.AsSpan().SequenceEqual(probes[p]))
                {
                    duplicate = true;
                    break;
                }
            }
            if (duplicate)
                continue;

            selectedBytes.Add(probes[p]);
            selected.Add((int)p);
        }

        selected.Sort();

        /* The result line.  The indices are reported in ascending probe order
         * so that the output is canonical; unused slots are -1. */
        var indices = new int[MaxSelected];
        for (int i = 0; i < MaxSelected; i++)
            indices[i] = i < selected.Count ? selected[i] : -1;

        if (!double.IsFinite(qualityMax) || !double.IsFinite(bestCross))
            Fail("non finite score");

        var line = string.Format(CultureInfo.InvariantCulture,
                                 "OK {0} {1} {2} {3} {4} {5} {6}",
                                 Format(qualityMax),
                                 Format(bestCross),
                                 selected.Count,
                                 indices[0], indices[1], indices[2], indices[3]);
        Console.Out.Write(line);
        Console.Out.Write('\n');
        Console.Out.Flush();
        return 0;
    }

    /* Read the exact input: the header first, then the payload that the
     * counts announce, then the end of the stream.  Nothing is read beyond
     * the announced size. */
    private static byte[] ReadInput()
    {
        var stdin = Console.OpenStandardInput();

        var header = new byte[HeaderBytes];
        if (!ReadExactly(stdin, header.AsSpan(0, HeaderBytes)))
            Fail("input shorter than the header");

        if (!header.AsSpan(0, Magic.Length).SequenceEqual(Magic))
            Fail("bad magic");

        uint probeCount = ReadU32Le(header, 8);
        uint referenceCount = ReadU32Le(header, 12);

        if (probeCount < 1 || probeCount > MaxFrames)
            Fail($"probe count {probeCount} out of range (1..{MaxFrames})");
        if (referenceCount > MaxFrames)
            Fail($"reference count {referenceCount} out of range (0..{MaxFrames})");

        long payload = ((long)probeCount + referenceCount) * FrameBytes;
        var input = new byte[HeaderBytes + payload];
        Buffer.BlockCopy(header, 0, input, 0, HeaderBytes);

        if (!ReadExactly(stdin, input.AsSpan(HeaderBytes, (int)payload)))
            Fail("input shorter than the announced frames");

        if (stdin.ReadByte() != -1)
            Fail("trailing bytes after the announced frames");

        return input;
    }

    /* Extract one template per frame.  A frame that carries no usable ridge
     * information has no template at all; a SourceAFIS failure aborts the
     * run. */
    private static FingerprintTemplate?[] Extract(byte[][] frames)
    {
        var templates = new FingerprintTemplate?[frames.Length];

        for (int i = 0; i < frames.Length; i++)
        {
            if (StdDev(frames[i]) < BlankStdDevThreshold)
                continue;

            var image = new FingerprintImage(Width, Height, Prepare(frames[i]),
                                             new FingerprintImageOptions { Dpi = 500 });
            templates[i] = new FingerprintTemplate(image);
        }

        return templates;
    }

    private static double Match(FingerprintTemplate a, FingerprintTemplate b) =>
        new FingerprintMatcher(a).Match(b);

    /* The validated image preparation: a per frame min/max linear contrast
     * stretch with integer rounding, then the inversion. */
    private static byte[] Prepare(byte[] src)
    {
        int min = 255;
        int max = 0;
        foreach (var b in src)
        {
            if (b < min)
                min = b;
            if (b > max)
                max = b;
        }

        var dst = new byte[src.Length];
        if (max > min)
        {
            int span = max - min;
            for (int i = 0; i < src.Length; i++)
                dst[i] = (byte)(((src[i] - min) * 255 + span / 2) / span);
        }
        else
        {
            Buffer.BlockCopy(src, 0, dst, 0, src.Length);
        }

        for (int i = 0; i < dst.Length; i++)
            dst[i] = (byte)(255 - dst[i]);

        return dst;
    }

    private static double StdDev(byte[] data)
    {
        double sum = 0.0;
        foreach (var b in data)
            sum += b;

        double mean = sum / data.Length;
        double squares = 0.0;
        foreach (var b in data)
        {
            double d = b - mean;
            squares += d * d;
        }

        return Math.Sqrt(squares / data.Length);
    }

    private static string Format(double value) => value.ToString("R", CultureInfo.InvariantCulture);

    private static byte[] Slice(byte[] data, long offset, int length)
    {
        var result = new byte[length];
        Buffer.BlockCopy(data, (int)offset, result, 0, length);
        return result;
    }

    private static uint ReadU32Le(byte[] data, int offset) =>
        (uint)(data[offset] |
               (data[offset + 1] << 8) |
               (data[offset + 2] << 16) |
               (data[offset + 3] << 24));

    private static bool ReadExactly(Stream stream, Span<byte> buffer)
    {
        int done = 0;
        while (done < buffer.Length)
        {
            int read = stream.Read(buffer[done..]);
            if (read <= 0)
                return false;
            done += read;
        }
        return true;
    }

    private static void Fail(string message)
    {
        Console.Error.WriteLine($"error: {message}");
        Console.Error.Flush();
        Environment.Exit(1);
    }
}
